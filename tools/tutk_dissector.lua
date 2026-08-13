-- ============================================================================
-- TUTK P2P Wireshark Dissector (Fixed IOCtrl Offset)
-- ============================================================================

local tutk_proto = Proto("TUTK", "TUTK P2P Protocol (Decrypted)")

-- --- Value Mappings ---
local IOTC_TYPES = {
    [0x0212] = "Handshake Req",
    [0x0213] = "Handshake Resp",
    [0x0301] = "NAT Punch",
    [0x0401] = "P2P Knock",
    [0x0402] = "P2P Knock Resp",
    [0x0407] = "Data Session Req",
    [0x0408] = "Data Session Resp",
    [0x0417] = "Close Session Req",
    [0x0427] = "Keepalive Req",
    [0x0428] = "Keepalive Resp",
    [0x0601] = "LAN Search Req",
    [0x0602] = "LAN Search Resp"
}

local AV_OPCODES = {
    [0x00] = "AV Login",
    [0x01] = "AV Login Resp",
    [0x03] = "Video Data",
    [0x04] = "Audio Data",
    [0x05] = "Audio Resend",
    [0x06] = "Video Resend",
    [0x07] = "Video Request",
    [0x08] = "Video OK",
    [0x0A] = "Video Lost",
    [0x10] = "IOCtrl Inner",
    [0x11] = "IOCtrl Inner ACK",
    [0x70] = "IOCtrl User (Command)",
    [0x71] = "IOCtrl User ACK (Response)"
}

local IO_TYPES = {
    [0x01FF] = "START",
    [0x0200] = "STOP",
    [0x0330] = "GET_FORMAT_REQ"
}

-- --- ProtoFields ---
local f_iotc_magic     = ProtoField.uint16("tutk.iotc.magic", "Magic Word", base.HEX)
local f_iotc_ver       = ProtoField.uint8("tutk.iotc.ver", "Version", base.DEC)
local f_iotc_flags1    = ProtoField.uint8("tutk.iotc.flags1", "Flags 1", base.HEX)
local f_iotc_len       = ProtoField.uint16("tutk.iotc.len", "Length", base.DEC)
local f_iotc_pktcnt    = ProtoField.uint16("tutk.iotc.pktcnt", "Packet Count", base.DEC)
local f_iotc_type      = ProtoField.uint16("tutk.iotc.type", "Type", base.HEX, IOTC_TYPES)
local f_iotc_flags2    = ProtoField.uint16("tutk.iotc.flags2", "Flags 2", base.HEX)
local f_iotc_rlysess   = ProtoField.uint16("tutk.iotc.rlysess", "Relay Session ID", base.DEC)
local f_iotc_chid      = ProtoField.uint8("tutk.iotc.chid", "Channel ID", base.DEC)
local f_iotc_reserved  = ProtoField.uint8("tutk.iotc.reserved", "Reserved / Mode", base.HEX)

local f_av_proto_type  = ProtoField.uint8("tutk.av.prototype", "Protocol Type", base.DEC)
local f_av_opcode      = ProtoField.uint8("tutk.av.opcode", "OpCode", base.HEX, AV_OPCODES)
local f_av_ver         = ProtoField.uint16("tutk.av.ver", "Version", base.DEC)
local f_av_frmno       = ProtoField.uint32("tutk.av.frmno", "Frame No", base.DEC)
local f_av_frmsize     = ProtoField.uint32("tutk.av.frmsize", "Frame Size", base.DEC)
local f_av_packno      = ProtoField.uint16("tutk.av.packno", "Pack No In Frame", base.DEC)
local f_av_frminfosize = ProtoField.uint16("tutk.av.frminfosize", "FrameInfo Size", base.DEC)
local f_av_payload_len = ProtoField.uint16("tutk.av.payload_len", "Payload Size", base.DEC)

local f_io_type        = ProtoField.uint32("tutk.io.type", "IOCtrl Type (ioType)", base.HEX, IO_TYPES)

tutk_proto.fields = {
    f_iotc_magic, f_iotc_ver, f_iotc_flags1, f_iotc_len, f_iotc_pktcnt,
    f_iotc_type, f_iotc_flags2, f_iotc_rlysess, f_iotc_chid, f_iotc_reserved,
    f_av_proto_type, f_av_opcode, f_av_ver, f_av_frmno, f_av_frmsize,
    f_av_packno, f_av_frminfosize, f_av_payload_len,
    f_io_type
}

-- --- Descrambler Logic ---
local PERM16 = {12, 10, 9, 16, 14, 11, 13, 15, 3, 2, 6, 1, 7, 5, 8, 4}
local PERM8  = {8, 5, 4, 3, 2, 7, 6, 1}
local PERM4  = {3, 4, 1, 2}
local PERM2  = {2, 1}
local TUTK_KEY = {67, 104, 97, 114, 108, 105, 101, 32, 105, 115, 32, 116, 104, 101, 32, 100}

local function rotl32(val, count)
    count = count % 32
    if count == 0 then return val end
    return ((val << count) | (val >> (32 - count))) & 0xFFFFFFFF
end

local function mix_bytes(src, size)
    local dst, perm = {}, nil
    if size == 16 then perm = PERM16
    elseif size == 8 then perm = PERM8
    elseif size == 4 then perm = PERM4
    elseif size == 2 then perm = PERM2 end

    if perm then
        for i = 1, size do dst[i] = src[perm[i]] end
    else
        for i = 1, size do dst[i] = src[i] end
    end
    return dst
end

local function rotate_words_left(src, offset_fn)
    local dst = {}
    for i = 0, 3 do
        local b0, b1, b2, b3 = src[i*4+1], src[i*4+2], src[i*4+3], src[i*4+4]
        local word = b0 + (b1 << 8) + (b2 << 16) + (b3 << 24)
        word = rotl32(word, offset_fn(i))
        dst[i*4+1] = word & 0xFF
        dst[i*4+2] = (word >> 8) & 0xFF
        dst[i*4+3] = (word >> 16) & 0xFF
        dst[i*4+4] = (word >> 24) & 0xFF
    end
    return dst
end

local function reverse_trans_code(raw_bytes, length)
    local out, offset = {}, 1
    while offset + 15 <= length do
        local block = {}
        for i = 1, 16 do block[i] = raw_bytes[offset + i - 1] end
        local s1 = rotate_words_left(block, function(i) return 3 + i * 4 end)
        local s2 = mix_bytes(s1, 16)
        for i = 1, 16 do s2[i] = s2[i] ~ TUTK_KEY[i] end
        local descrambled = rotate_words_left(s2, function(i) return 1 + i * 4 end)
        for i = 1, 16 do out[offset + i - 1] = descrambled[i] end
        offset = offset + 16
    end
    local remain = length - offset + 1
    if remain > 0 then
        local tail_in = {}
        for i = 1, remain do tail_in[i] = raw_bytes[offset + i - 1] end
        local mixed = mix_bytes(tail_in, remain)
        for i = 1, remain do out[offset + i - 1] = mixed[i] ~ TUTK_KEY[i] end
    end
    return out
end

-- --- Main Dissector ---
function tutk_proto.dissector(buffer, pinfo, tree)
    local pkt_len = buffer:len()
    if pkt_len < 16 then return end

    local raw_bytes = {}
    for i = 0, pkt_len - 1 do raw_bytes[i + 1] = buffer(i, 1):uint() end
    local dec_bytes = reverse_trans_code(raw_bytes, pkt_len)

    local ba = ByteArray.new()
    ba:set_size(pkt_len)
    for i = 1, pkt_len do ba:set_index(i - 1, dec_bytes[i]) end
    local dec_tvb = ba:tvb("Decrypted TUTK")

    -- Check Magic Word
    local magic = dec_tvb(0, 2):le_uint()
    if magic ~= 0x0204 then return end

    pinfo.cols.protocol = "TUTK_P2P"

    -- 1. IOTC Header (16 bytes: offsets 0..15)
    local iotc_tree = tree:add(tutk_proto, dec_tvb(0, 16), "IOTC Header")
    iotc_tree:add_le(f_iotc_magic, dec_tvb(0, 2))
    iotc_tree:add(f_iotc_ver, dec_tvb(2, 1))
    iotc_tree:add(f_iotc_flags1, dec_tvb(3, 1))
    iotc_tree:add_le(f_iotc_len, dec_tvb(4, 2))
    iotc_tree:add_le(f_iotc_pktcnt, dec_tvb(6, 2))
    
    local cmd_type = dec_tvb(8, 2):le_uint()
    iotc_tree:add_le(f_iotc_type, dec_tvb(8, 2))
    iotc_tree:add_le(f_iotc_flags2, dec_tvb(10, 2))
    iotc_tree:add_le(f_iotc_rlysess, dec_tvb(12, 2))
    iotc_tree:add(f_iotc_chid, dec_tvb(14, 1))
    iotc_tree:add(f_iotc_reserved, dec_tvb(15, 1))

    local type_name = IOTC_TYPES[cmd_type] or string.format("0x%04X", cmd_type)
    pinfo.cols.info:set(string.format("IOTC: %s (Len: %d)", type_name, dec_tvb(4, 2):le_uint()))

    -- 2. AVPacket Head (24 bytes: offsets 16..39)
    if (cmd_type == 0x0407 or cmd_type == 0x0408) and pkt_len >= 40 then
        local av_tvb = dec_tvb(16, 24)
        local av_tree = tree:add(tutk_proto, av_tvb, "AVPacket Head")
        
        av_tree:add(f_av_proto_type, av_tvb(0, 1))
        local op_code = av_tvb(1, 1):uint()
        av_tree:add(f_av_opcode, av_tvb(1, 1))
        av_tree:add_le(f_av_ver, av_tvb(2, 2))
        
        local frame_no = av_tvb(4, 4):le_uint()
        av_tree:add_le(f_av_frmno, av_tvb(4, 4))
        av_tree:add_le(f_av_frmsize, av_tvb(8, 4))
        av_tree:add_le(f_av_packno, av_tvb(12, 2))
        av_tree:add_le(f_av_frminfosize, av_tvb(14, 2))
        av_tree:add_le(f_av_payload_len, av_tvb(16, 2))

        local op_name = AV_OPCODES[op_code] or string.format("0x%02X", op_code)
        pinfo.cols.info:append(string.format(" | AV: %s", op_name))

        -- 3. IOCtrl Command Parsing (ioType directly at offset 40 / 0x28)
        if (op_code == 0x70 or op_code == 0x71) and pkt_len >= 44 then
            local io_type = dec_tvb(40, 4):le_uint()
            
            local io_tree = tree:add(tutk_proto, dec_tvb(40, 4), "IOCtrl Header")
            io_tree:add_le(f_io_type, dec_tvb(40, 4))

            local io_name = IO_TYPES[io_type] or string.format("0x%04X", io_type)
            pinfo.cols.info:append(string.format(" | IOCtrl: %s", io_name))

            -- All bytes from offset 44 onward are treated as raw payload data
            if pkt_len > 44 then
                local payload_len = pkt_len - 44
                tree:add(tutk_proto, dec_tvb(44, payload_len), string.format("IOCtrl Data Payload (%d bytes)", payload_len))
            end
        elseif pkt_len > 40 then
            tree:add(tutk_proto, dec_tvb(40, pkt_len - 40), string.format("AV Payload Data (%d bytes)", pkt_len - 40))
        end
    end
end

DissectorTable.get("udp.port"):add_for_decode_as(tutk_proto)