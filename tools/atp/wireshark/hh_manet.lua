--[[
hh_manet.lua — Wireshark dissector for the HH-SDR MANET beacon and
routing-update payloads.

WHAT THIS DECODES
  The two payload layouts defined by src/radio/wire.c:
    beacon          48 bytes, fixed, protocol_version 1
    routing update  5 + 11 * count bytes, count <= 24
  Both are little-endian with no padding between fields.

  These layouts are THE IMPLEMENTATION'S OWN CHOICE, not a specification: the
  architecture leaves the over-the-air byte layout TBD pending a PHY decision.
  If wire.c changes, this file must change with it — tools/atp/wireshark/
  check-dissector.sh compares the two on every ctest run, so drift fails the
  build instead of going unnoticed.

WHAT THIS DOES NOT DECODE
  - Any frame header. The frame kind, source and destination are hh_frame_t
    struct fields that are never serialised; no air frame header exists
    (unknown.md U-16).
  - Data-frame payloads, which are opaque user bytes.
  - ICD-2 control traffic (unknown.md U-01) or ICD-1 DMA descriptors (U-06).

HOW FRAMES REACH IT
  Captures use private pcap link types, standing in for the missing frame
  header: DLT_USER0 (147) = beacon, DLT_USER1 (148) = routing update.
  tools/atp/wireshark/hh_wire_pcapgen writes such captures from the real
  encoder. This is a test-tooling convention, not an air format.

USE
  tshark -X lua_script:hh_manet.lua -r beacons.pcap -V
  or copy into ~/.local/lib/wireshark/plugins/ for the Wireshark GUI.

STATUS FIELD
  Every frame gets hhbeacon.status / hhroute.status, with the same verdicts
  the C decoder reaches:
    ok                   decoded fully
    unsupported_version  protocol_version is not 1; nothing past it is decoded,
                         exactly as hh_beacon_decode refuses to interpret it
    malformed            too short, or an entry count out of range
    invalid_node         well-formed, but the node/sender id is 0, which is
                         the invalid/broadcast sentinel
--]]

local PROTOCOL_VERSION = 1
local BEACON_LEN = 48
local ROUTE_HDR_LEN = 5
local ROUTE_ENTRY_LEN = 11
local ROUTE_MAX_ENTRIES = 24

-- Scale factors from wire.c. Coordinates reuse wire.c's 0.1-unit encoding;
-- their physical unit is not defined anywhere, so it is not claimed here.
local POS_SCALE = 10.0
local FRAC_SCALE = 10000.0

------------------------------------------------------------------------------
-- Beacon
------------------------------------------------------------------------------

local beacon = Proto("hhbeacon", "HH-SDR MANET Beacon (implementation format)")

local bf = {
    node_id      = ProtoField.uint32("hhbeacon.node_id", "Node ID", base.DEC),
    version      = ProtoField.uint16("hhbeacon.protocol_version", "Protocol version", base.DEC),
    seq          = ProtoField.uint32("hhbeacon.sequence_no", "Sequence number", base.DEC),
    timestamp    = ProtoField.uint64("hhbeacon.timestamp_ms", "Timestamp (ms)", base.DEC),
    caps         = ProtoField.uint32("hhbeacon.capabilities", "Capabilities (encoding TBD)", base.HEX),
    radio_caps   = ProtoField.uint32("hhbeacon.radio_caps", "Radio capabilities (encoding TBD)", base.HEX),
    waveforms    = ProtoField.uint32("hhbeacon.supported_waveforms", "Supported waveforms (encoding TBD)", base.HEX),
    freq         = ProtoField.uint32("hhbeacon.channel_freq_hz", "Channel frequency (Hz)", base.DEC),
    flags        = ProtoField.uint8("hhbeacon.flags", "Flags", base.HEX),
    f_routing    = ProtoField.bool("hhbeacon.routing_capable", "Routing capable", 8, nil, 0x01),
    f_position   = ProtoField.bool("hhbeacon.position_valid", "Position valid", 8, nil, 0x02),
    f_power      = ProtoField.bool("hhbeacon.power_valid", "Power valid", 8, nil, 0x04),
    pos_x        = ProtoField.float("hhbeacon.position_x", "Position X (unit TBD)"),
    pos_y        = ProtoField.float("hhbeacon.position_y", "Position Y (unit TBD)"),
    pos_z        = ProtoField.float("hhbeacon.position_z", "Position Z (unit TBD)"),
    battery      = ProtoField.float("hhbeacon.power_battery", "Battery (fraction 0..1)"),
    reserved     = ProtoField.uint8("hhbeacon.reserved", "Reserved", base.HEX),
    padding      = ProtoField.bytes("hhbeacon.padding", "Padding"),
    status       = ProtoField.string("hhbeacon.status", "Decode status"),
}
beacon.fields = bf

local be = {
    short   = ProtoExpert.new("hhbeacon.expert.short", "Beacon shorter than 48 bytes",
                              expert.group.MALFORMED, expert.severity.ERROR),
    version = ProtoExpert.new("hhbeacon.expert.version", "Unsupported protocol version; rest not decoded",
                              expert.group.PROTOCOL, expert.severity.WARN),
    node0   = ProtoExpert.new("hhbeacon.expert.node0", "Node ID 0 is the invalid/broadcast sentinel",
                              expert.group.PROTOCOL, expert.severity.ERROR),
    unused  = ProtoExpert.new("hhbeacon.expert.unused", "Optional field slot present but flagged invalid",
                              expert.group.COMMENTS_GROUP, expert.severity.CHAT),
}
beacon.experts = { be.short, be.version, be.node0, be.unused }

-- A signed 0.1-unit coordinate or an unsigned 1e-4 fraction, shown as the
-- decoded value with the raw bytes highlighted.
local function add_scaled(tree, field, range, value, valid)
    local item = tree:add(field, range, value)
    if not valid then item:add_proto_expert_info(be.unused) end
end

function beacon.dissector(tvb, pinfo, root)
    pinfo.cols.protocol = "HH-BEACON"
    local len = tvb:len()
    local tree = root:add(beacon, tvb(), "HH-SDR MANET Beacon")

    if len < BEACON_LEN then
        tree:add(bf.status, "malformed"):set_generated()
        tree:add_proto_expert_info(be.short)
        pinfo.cols.info = string.format("Beacon (malformed: %d of %d bytes)", len, BEACON_LEN)
        return len
    end

    local node_id = tvb(0, 4):le_uint()
    local version = tvb(4, 2):le_uint()
    local node_item = tree:add_le(bf.node_id, tvb(0, 4))
    local ver_item = tree:add_le(bf.version, tvb(4, 2))

    -- Mirrors hh_beacon_decode: the version is checked before any later field
    -- is interpreted, because a different version may lay them out differently.
    if version ~= PROTOCOL_VERSION then
        tree:add(bf.status, "unsupported_version"):set_generated()
        ver_item:add_proto_expert_info(be.version)
        pinfo.cols.info = string.format("Beacon node=%u (unsupported version %u)", node_id, version)
        return len
    end

    local seq = tvb(6, 4):le_uint()
    tree:add_le(bf.seq, tvb(6, 4))
    tree:add_le(bf.timestamp, tvb(10, 8))
    tree:add_le(bf.caps, tvb(18, 4))
    tree:add_le(bf.radio_caps, tvb(22, 4))
    tree:add_le(bf.waveforms, tvb(26, 4))
    tree:add_le(bf.freq, tvb(30, 4))

    local flags = tvb(34, 1):uint()
    local ftree = tree:add(bf.flags, tvb(34, 1))
    ftree:add(bf.f_routing, tvb(34, 1))
    ftree:add(bf.f_position, tvb(34, 1))
    ftree:add(bf.f_power, tvb(34, 1))
    local pos_valid = bit32.band(flags, 0x02) ~= 0
    local pwr_valid = bit32.band(flags, 0x04) ~= 0

    add_scaled(tree, bf.pos_x, tvb(35, 2), tvb(35, 2):le_int() / POS_SCALE, pos_valid)
    add_scaled(tree, bf.pos_y, tvb(37, 2), tvb(37, 2):le_int() / POS_SCALE, pos_valid)
    add_scaled(tree, bf.pos_z, tvb(39, 2), tvb(39, 2):le_int() / POS_SCALE, pos_valid)
    add_scaled(tree, bf.battery, tvb(41, 2), tvb(41, 2):le_uint() / FRAC_SCALE, pwr_valid)
    tree:add(bf.reserved, tvb(43, 1))
    tree:add(bf.padding, tvb(44, BEACON_LEN - 44))

    if node_id == 0 then
        tree:add(bf.status, "invalid_node"):set_generated()
        node_item:add_proto_expert_info(be.node0)
        pinfo.cols.info = string.format("Beacon node=0 (invalid) seq=%u", seq)
    else
        tree:add(bf.status, "ok"):set_generated()
        pinfo.cols.info = string.format("Beacon node=%u seq=%u", node_id, seq)
    end
    return BEACON_LEN
end

------------------------------------------------------------------------------
-- Routing update
------------------------------------------------------------------------------

local route = Proto("hhroute", "HH-SDR MANET Routing Update (implementation format)")

local rf = {
    sender     = ProtoField.uint32("hhroute.sender", "Sender", base.DEC),
    count      = ProtoField.uint8("hhroute.count", "Entry count", base.DEC),
    entry      = ProtoField.none("hhroute.entry", "Entry"),
    originator = ProtoField.uint32("hhroute.originator", "Originator", base.DEC),
    seq        = ProtoField.uint32("hhroute.sequence_no", "Sequence number", base.DEC),
    hops       = ProtoField.uint8("hhroute.hop_count", "Hop count", base.DEC),
    metric     = ProtoField.float("hhroute.metric", "Metric (0..1, sender's composite)"),
    status     = ProtoField.string("hhroute.status", "Decode status"),
}
route.fields = rf

local re = {
    short = ProtoExpert.new("hhroute.expert.short", "Routing update shorter than its entry count requires",
                            expert.group.MALFORMED, expert.severity.ERROR),
    count = ProtoExpert.new("hhroute.expert.count", "Entry count above the maximum of 24",
                            expert.group.MALFORMED, expert.severity.ERROR),
    node0 = ProtoExpert.new("hhroute.expert.node0", "Sender 0 is the invalid/broadcast sentinel",
                            expert.group.PROTOCOL, expert.severity.ERROR),
}
route.experts = { re.short, re.count, re.node0 }

function route.dissector(tvb, pinfo, root)
    pinfo.cols.protocol = "HH-ROUTE"
    local len = tvb:len()
    local tree = root:add(route, tvb(), "HH-SDR MANET Routing Update")

    if len < ROUTE_HDR_LEN then
        tree:add(rf.status, "malformed"):set_generated()
        tree:add_proto_expert_info(re.short)
        pinfo.cols.info = "Routing update (malformed: header cut short)"
        return len
    end

    local sender = tvb(0, 4):le_uint()
    local count = tvb(4, 1):uint()
    local sender_item = tree:add_le(rf.sender, tvb(0, 4))
    local count_item = tree:add(rf.count, tvb(4, 1))

    -- Same order of checks as hh_route_update_decode.
    if count > ROUTE_MAX_ENTRIES then
        tree:add(rf.status, "malformed"):set_generated()
        count_item:add_proto_expert_info(re.count)
        pinfo.cols.info = string.format("Routing update sender=%u (malformed: count %u)", sender, count)
        return len
    end
    if len < ROUTE_HDR_LEN + count * ROUTE_ENTRY_LEN then
        tree:add(rf.status, "malformed"):set_generated()
        count_item:add_proto_expert_info(re.short)
        pinfo.cols.info = string.format("Routing update sender=%u (malformed: %d bytes for %u entries)",
                                        sender, len, count)
        return len
    end

    for i = 0, count - 1 do
        local off = ROUTE_HDR_LEN + i * ROUTE_ENTRY_LEN
        local etree = tree:add(rf.entry, tvb(off, ROUTE_ENTRY_LEN))
        etree:set_text(string.format("Entry %d: originator=%u hops=%u",
                                     i, tvb(off, 4):le_uint(), tvb(off + 8, 1):uint()))
        etree:add_le(rf.originator, tvb(off, 4))
        etree:add_le(rf.seq, tvb(off + 4, 4))
        etree:add(rf.hops, tvb(off + 8, 1))
        etree:add(rf.metric, tvb(off + 9, 2), tvb(off + 9, 2):le_uint() / FRAC_SCALE)
    end

    if sender == 0 then
        tree:add(rf.status, "invalid_node"):set_generated()
        sender_item:add_proto_expert_info(re.node0)
        pinfo.cols.info = string.format("Routing update sender=0 (invalid) entries=%u", count)
    else
        tree:add(rf.status, "ok"):set_generated()
        pinfo.cols.info = string.format("Routing update sender=%u entries=%u", sender, count)
    end
    return ROUTE_HDR_LEN + count * ROUTE_ENTRY_LEN
end

------------------------------------------------------------------------------
-- Registration on the private link types
------------------------------------------------------------------------------

-- wtap_encaps is the current name; wtap is the pre-3.x one it replaced.
local encaps = wtap_encaps or wtap
local wtap_table = DissectorTable.get("wtap_encap")
wtap_table:add(encaps.USER0, beacon)
wtap_table:add(encaps.USER1, route)
