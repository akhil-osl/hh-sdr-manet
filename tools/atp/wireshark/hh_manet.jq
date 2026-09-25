# hh_manet.jq — turn tshark's JSON for hh_manet.lua into one flat record per
# frame. Shared by check-dissector.sh (validation) and atp-decode.sh
# (evidence), so both read the dissector's output the same way.
#
# Input:  tshark -X lua_script:hh_manet.lua -T json --no-duplicate-keys
# Output: [ {frame, proto: "beacon"|"route"|null, status, fields?}, ... ]
#
# "fields" is present only for status "ok", matching expected.json written by
# hh_wire_pcapgen. Values are converted back to numbers — tshark prints every
# field as a string, and the capability fields in hex — except timestamp_ms,
# which stays a string because a uint64 does not fit a jq number exactly.

def hex:
    ltrimstr("0x") | ascii_downcase | explode
    | reduce .[] as $c (0; . * 16 + (if $c >= 97 then $c - 87 else $c - 48 end));

def beacon_fields:
    { node_id:             (.["hhbeacon.node_id"] | tonumber),
      protocol_version:    (.["hhbeacon.protocol_version"] | tonumber),
      sequence_no:         (.["hhbeacon.sequence_no"] | tonumber),
      timestamp_ms:         .["hhbeacon.timestamp_ms"],
      capabilities:        (.["hhbeacon.capabilities"] | hex),
      radio_caps:          (.["hhbeacon.radio_caps"] | hex),
      supported_waveforms: (.["hhbeacon.supported_waveforms"] | hex),
      channel_freq_hz:     (.["hhbeacon.channel_freq_hz"] | tonumber),
      routing_capable:     (.["hhbeacon.flags_tree"]["hhbeacon.routing_capable"] | tonumber),
      position_valid:      (.["hhbeacon.flags_tree"]["hhbeacon.position_valid"] | tonumber),
      power_valid:         (.["hhbeacon.flags_tree"]["hhbeacon.power_valid"] | tonumber),
      position_x:          (.["hhbeacon.position_x"] | tonumber),
      position_y:          (.["hhbeacon.position_y"] | tonumber),
      position_z:          (.["hhbeacon.position_z"] | tonumber),
      power_battery:       (.["hhbeacon.power_battery"] | tonumber) };

def route_fields:
    { sender:  (.["hhroute.sender"] | tonumber),
      count:   (.["hhroute.count"] | tonumber),
      # One entry arrives as an object, several as an array, none as absent.
      entries: ([.["hhroute.entry"] // empty] | flatten
                | map({ originator:  (.["hhroute.originator"] | tonumber),
                        sequence_no: (.["hhroute.sequence_no"] | tonumber),
                        hop_count:   (.["hhroute.hop_count"] | tonumber),
                        metric:      (.["hhroute.metric"] | tonumber) })) };

def hh_frames:
    map(._source.layers as $l
        | ($l.frame["frame.number"] | tonumber) as $n
        | if $l.hhbeacon then
              { frame: $n, proto: "beacon", status: $l.hhbeacon["hhbeacon.status"] }
              + (if $l.hhbeacon["hhbeacon.status"] == "ok"
                 then { fields: ($l.hhbeacon | beacon_fields) } else {} end)
          elif $l.hhroute then
              { frame: $n, proto: "route", status: $l.hhroute["hhroute.status"] }
              + (if $l.hhroute["hhroute.status"] == "ok"
                 then { fields: ($l.hhroute | route_fields) } else {} end)
          else
              { frame: $n, proto: null, status: "not_hh" }
          end);
