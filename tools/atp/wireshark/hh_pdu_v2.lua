--[[
hh_pdu_v2.lua - Wireshark dissector for the HH-SDR MAC PDU v2.

WHAT THIS DECODES
  The PDU of MAC frame specification v2, part 4: Frame Control, PDU sequence,
  Net ID, PDU destination, Source, control block (time quality, flags,
  break-in, Block ACK entries, link reports, slot claims, beacon body, queue
  report), the messages with their sub-headers and fragment headers, and the
  CRC-32, which it recomputes and checks. All fields are little-endian.
  MAC command bodies are shown as a command id plus raw bytes.

WHAT THIS DOES NOT DECODE
  - Secured PDUs (Frame Control security bit) and extension elements. The
    header is shown and the status is "unsupported".
  - Message data. It is shown as bytes.

HOW FRAMES REACH IT
  Captures use the private pcap link type DLT_USER2 (149), one PDU per record
  with no outer header. hh-slot-sim --pcap writes such captures. This is a
  test-tooling convention, not an air format. The existing beacon and routing
  dissector (hh_manet.lua) uses DLT_USER0 and DLT_USER1.

USE
  tshark -X lua_script:hh_pdu_v2.lua -r run.pcap -V

STATUS FIELD (hhpdu.status)
  ok                 decoded fully, CRC good
  bad_crc            decoded, but the CRC-32 does not match
  bad_frame_control  Frame Control differs from section 4.2
  unsupported        security or extension elements present
  malformed          a length runs past the end of the PDU
--]]

local pdu = Proto("hhpdu", "HH-SDR MAC PDU v2")

local CLASS = { [0] = "control", "emergency", "voice", "video", "data" }
local PROTO = {
    [0x01] = "IPv6", [0x02] = "IPv4", [0x03] = "IP (ROHC)", [0x20] = "MAC command",
    [0x30] = "network beacon", [0x31] = "routing", [0x32] = "network data (opaque)",
    [0xF0] = "test text", [0xFF] = "escape (EtherType)",
}
local COMMAND = { [1] = "entry request", [2] = "entry grant", [3] = "entry reject", [4] = "leave" }

local f = pdu.fields
f.fc        = ProtoField.uint16("hhpdu.frame_control", "Frame Control", base.HEX)
f.more      = ProtoField.bool("hhpdu.more_data", "More data")
f.seq       = ProtoField.uint8("hhpdu.seq", "PDU sequence", base.DEC)
f.net       = ProtoField.uint16("hhpdu.net_id", "Net ID", base.HEX)
f.dst       = ProtoField.uint16("hhpdu.dst", "PDU destination", base.HEX)
f.src       = ProtoField.uint16("hhpdu.src", "Source", base.HEX)
f.tq        = ProtoField.uint8("hhpdu.time_quality", "Time quality", base.HEX)
f.flags     = ProtoField.uint8("hhpdu.flags", "Flags", base.HEX)
f.breakin   = ProtoField.bool("hhpdu.breakin", "Break-in active")
f.hops      = ProtoField.uint8("hhpdu.breakin_hops", "Break-in hop count", base.DEC)
f.invoker   = ProtoField.uint16("hhpdu.breakin_invoker", "Break-in invoker", base.HEX)
f.nback     = ProtoField.uint8("hhpdu.block_acks", "Block ACK entries", base.DEC)
f.nlink     = ProtoField.uint8("hhpdu.link_reports", "Link report entries", base.DEC)
f.bitmap_l  = ProtoField.uint8("hhpdu.slot_bitmap_len", "Slot bitmap length L", base.DEC)
f.claims    = ProtoField.bytes("hhpdu.claims", "Own slot claims")
f.slots     = ProtoField.string("hhpdu.slots", "Own slots")
f.back_nb   = ProtoField.uint16("hhpdu.back.neighbor", "Block ACK neighbour", base.HEX)
f.back_st   = ProtoField.uint8("hhpdu.back.start", "Block ACK start sequence", base.DEC)
f.back_bm   = ProtoField.uint32("hhpdu.back.bitmap", "Block ACK bitmap", base.HEX)
f.link_nb   = ProtoField.uint16("hhpdu.link.neighbor", "Link report neighbour", base.HEX)
f.link_err  = ProtoField.uint8("hhpdu.link.error_rate", "Error rate (0.5 % steps)", base.DEC)
f.link_snr  = ProtoField.uint8("hhpdu.link.snr", "SNR (0.25 dB steps)", base.DEC)
f.bc_found  = ProtoField.uint32("hhpdu.beacon.founding_superframe", "Founding superframe", base.DEC)
f.bc_fdr    = ProtoField.uint16("hhpdu.beacon.founder", "Founder", base.HEX)
f.bc_sf     = ProtoField.uint32("hhpdu.beacon.superframe", "Superframe number", base.DEC)
f.bc_entry  = ProtoField.uint8("hhpdu.beacon.entry_slot", "Entry slot", base.DEC)
f.bc_win    = ProtoField.uint8("hhpdu.beacon.window", "Window advice", base.DEC)
f.bc_mem    = ProtoField.uint8("hhpdu.beacon.members", "Member count", base.DEC)
f.bc_heard  = ProtoField.bytes("hhpdu.beacon.heard", "Slots heard in use")
f.qreport   = ProtoField.bytes("hhpdu.qreport", "Queue report")
f.nmsg      = ProtoField.uint8("hhpdu.messages", "Messages", base.DEC)
f.m_proto   = ProtoField.uint8("hhpdu.msg.proto", "Protocol ID", base.HEX, PROTO)
f.m_dst     = ProtoField.uint16("hhpdu.msg.dst", "Message destination", base.HEX)
f.m_len     = ProtoField.uint16("hhpdu.msg.length", "Message length", base.DEC)
f.m_cls     = ProtoField.uint8("hhpdu.msg.class", "Traffic class", base.DEC, CLASS)
f.m_frag    = ProtoField.bool("hhpdu.msg.fragment", "Fragment header follows")
f.m_ack     = ProtoField.bool("hhpdu.msg.ack_requested", "ACK requested")
f.m_lseq    = ProtoField.uint8("hhpdu.msg.link_seq", "Link sequence", base.DEC)
f.m_dsize   = ProtoField.uint16("hhpdu.msg.datagram_size", "Datagram size", base.DEC)
f.m_dtag    = ProtoField.uint16("hhpdu.msg.datagram_tag", "Datagram tag", base.HEX)
f.m_off     = ProtoField.uint8("hhpdu.msg.offset", "Offset (8-byte units)", base.DEC)
f.m_cmd     = ProtoField.uint8("hhpdu.msg.command", "MAC command", base.DEC, COMMAND)
f.m_data    = ProtoField.bytes("hhpdu.msg.data", "Message data")
f.crc       = ProtoField.uint32("hhpdu.crc", "CRC-32", base.HEX)
f.crc_calc  = ProtoField.uint32("hhpdu.crc_calculated", "CRC-32 calculated", base.HEX)
f.status    = ProtoField.string("hhpdu.status", "Status")

-- Bit helpers that work on Lua 5.2 (bit32) and on 5.3 or later (operators).
local xor32 = (bit32 and bit32.bxor) or load("return function(a, b) return a ~ b end")()
local band = (bit32 and bit32.band) or load("return function(a, b) return a & b end")()
local shr = (bit32 and bit32.rshift) or load("return function(a, n) return a >> n end")()

local crc_table = {}
for i = 0, 255 do
    local c = i
    for _ = 1, 8 do
        if c % 2 == 1 then c = xor32(math.floor(c / 2), 0xEDB88320) else c = math.floor(c / 2) end
    end
    crc_table[i] = c
end

local function crc32(str)
    local crc = 0xFFFFFFFF
    for i = 1, #str do
        crc = xor32(crc_table[xor32(crc % 256, str:byte(i))], math.floor(crc / 256))
    end
    return xor32(crc, 0xFFFFFFFF)
end

local function set_status(tree, pinfo, st)
    tree:add(f.status, st)
    pinfo.cols.info:append(" [" .. st .. "]")
end

function pdu.dissector(tvb, pinfo, root)
    local len = tvb:len()
    local tree = root:add(pdu, tvb(), "HH-SDR MAC PDU v2")
    pinfo.cols.protocol = "HHPDU"
    pinfo.cols.info = "MAC PDU"

    if len < 19 then
        set_status(tree, pinfo, "malformed")
        return
    end

    local end_ = len - 4
    local pos = 0
    local malformed = false
    local function have(n) if pos + n > end_ then malformed = true; return false end return true end

    local fc = tvb(0, 2):le_uint()
    local fct = tree:add_le(f.fc, tvb(0, 2))
    fct:add(f.more, tvb(0, 2), band(fc, 0x10) ~= 0)
    pos = 2
    tree:add(f.seq, tvb(pos, 1)); pos = pos + 1
    tree:add_le(f.net, tvb(pos, 2)); pos = pos + 2
    tree:add_le(f.dst, tvb(pos, 2)); pos = pos + 2
    local src = tvb(pos, 2):le_uint()
    tree:add_le(f.src, tvb(pos, 2)); pos = pos + 2
    pinfo.cols.info = string.format("MAC PDU from 0x%04X", src)

    local status = "ok"
    local unsupported = band(fc, 0x0008) ~= 0 or band(fc, 0x0200) ~= 0
    local fc_ok = band(fc, 0xFFEF) == 0xA841
    if unsupported then status = "unsupported"
    elseif not fc_ok then status = "bad_frame_control" end

    if not unsupported and fc_ok then
        -- Control block
        if have(6) then
            tree:add(f.tq, tvb(pos, 1))
            local fl = tvb(pos + 1, 1):uint()
            local ft = tree:add(f.flags, tvb(pos + 1, 1))
            ft:add(f.breakin, tvb(pos + 1, 1), band(fl, 1) ~= 0)
            ft:add(f.hops, tvb(pos + 1, 1), band(shr(fl, 1), 7))
            tree:add_le(f.invoker, tvb(pos + 2, 2))
            local counts = tvb(pos + 4, 1):uint()
            local nback, nlink = band(counts, 15), shr(counts, 4)
            tree:add(f.nback, tvb(pos + 4, 1), nback)
            tree:add(f.nlink, tvb(pos + 4, 1), nlink)
            local L = tvb(pos + 5, 1):uint()
            tree:add(f.bitmap_l, tvb(pos + 5, 1))
            pos = pos + 6
            if L > 8 or not have(L) then malformed = true else
                if L > 0 then
                    tree:add(f.claims, tvb(pos, L))
                    local slots = {}
                    for b = 0, L - 1 do
                        local byte = tvb(pos + b, 1):uint()
                        for bit = 0, 7 do
                            if band(shr(byte, bit), 1) == 1 then slots[#slots + 1] = tostring(b * 8 + bit) end
                        end
                    end
                    tree:add(f.slots, #slots > 0 and table.concat(slots, ",") or "none")
                end
                pos = pos + L
                for _ = 1, nback do
                    if not have(7) then break end
                    local t = tree:add(f.back_nb, tvb(pos, 2), tvb(pos, 2):le_uint())
                    t:add(f.back_st, tvb(pos + 2, 1))
                    t:add_le(f.back_bm, tvb(pos + 3, 4))
                    pos = pos + 7
                end
                for _ = 1, nlink do
                    if not have(4) then break end
                    local t = tree:add(f.link_nb, tvb(pos, 2), tvb(pos, 2):le_uint())
                    t:add(f.link_err, tvb(pos + 2, 1))
                    t:add(f.link_snr, tvb(pos + 3, 1))
                    pos = pos + 4
                end
                if band(fl, 0x10) ~= 0 and have(13 + L) then
                    local bt = tree:add(pdu, tvb(pos, 13 + L), "Beacon body")
                    bt:add_le(f.bc_found, tvb(pos, 4))
                    bt:add_le(f.bc_fdr, tvb(pos + 4, 2))
                    bt:add_le(f.bc_sf, tvb(pos + 6, 4))
                    bt:add(f.bc_entry, tvb(pos + 10, 1))
                    bt:add(f.bc_win, tvb(pos + 11, 1))
                    bt:add(f.bc_mem, tvb(pos + 12, 1))
                    if L > 0 then bt:add(f.bc_heard, tvb(pos + 13, L)) end
                    pos = pos + 13 + L
                elseif band(fl, 0x10) ~= 0 then malformed = true end
                if band(fl, 0x20) ~= 0 then
                    if have(5) then tree:add(f.qreport, tvb(pos, 5)); pos = pos + 5 end
                end
            end
        end

        -- Messages
        local n = 0
        while not malformed and pos < end_ do
            if not have(5) then break end
            local word = tvb(pos + 3, 2):le_uint()
            local mlen, cls = band(word, 0x7FF), band(shr(word, 11), 7)
            local frag, ack = band(word, 0x4000) ~= 0, band(word, 0x8000) ~= 0
            local hdr = 5 + (ack and 1 or 0) + (frag and 5 or 0)
            if not have(hdr + mlen) then break end
            local m = tree:add(pdu, tvb(pos, hdr + mlen), "Message " .. n)
            m:add(f.m_proto, tvb(pos, 1))
            m:add_le(f.m_dst, tvb(pos + 1, 2))
            m:add(f.m_len, tvb(pos + 3, 2), mlen)
            m:add(f.m_cls, tvb(pos + 3, 2), cls)
            m:add(f.m_frag, tvb(pos + 3, 2), frag)
            m:add(f.m_ack, tvb(pos + 3, 2), ack)
            local o = pos + 5
            if ack then m:add(f.m_lseq, tvb(o, 1)); o = o + 1 end
            if frag then
                m:add_le(f.m_dsize, tvb(o, 2), band(tvb(o, 2):le_uint(), 0x7FF))
                m:add_le(f.m_dtag, tvb(o + 2, 2))
                m:add(f.m_off, tvb(o + 4, 1))
                o = o + 5
            end
            if tvb(pos, 1):uint() == 0x20 and mlen > 0 then m:add(f.m_cmd, tvb(o, 1)) end
            if mlen > 0 then m:add(f.m_data, tvb(o, mlen)) end
            pos = pos + hdr + mlen
            n = n + 1
        end
        tree:add(f.nmsg, n):set_generated()
        if malformed then status = "malformed" end
    end

    -- CRC-32 over everything before it
    local got = tvb(end_, 4):le_uint()
    local calc = crc32(tvb:raw(0, end_))
    tree:add_le(f.crc, tvb(end_, 4))
    tree:add(f.crc_calc, calc):set_generated()
    if status == "ok" and got ~= calc then status = "bad_crc" end
    set_status(tree, pinfo, status)
end

local encaps = wtap_encaps or wtap
DissectorTable.get("wtap_encap"):add(encaps.USER2, pdu)
