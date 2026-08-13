#pragma once

#include <cstdint>
#include <vector>
#include <cstring>  // memcpy
#include <bit>      // std::endian
#include <span>     // std::span

#include <string_view>
#include <optional>
#include <boost/asio/ip/udp.hpp>

namespace iotc
{
    using u8    = std::uint8_t;
    using u16   = std::uint16_t;
    using u32   = std::uint32_t;
    enum class IotcPacketType : u16
    {
        // Session / Auth
        HandshakeReq        = 0x212,    //
        HandshakeResp       = 0x213,    //
        RegisterNatType     = 0x1005,   //

        // hole punching
        NatPunch            = 0x301,    //
        P2PKnock            = 0x401,    // P2P_KNOCK
        P2PKnockResp        = 0x402,    //
        P2PKnockRespResp    = 0x404,    //

        // session control
        DataSessionReq      = 0x407,    // P2P_PACKET_C2D
        DataSessionResp     = 0x408,    // P2P_PACKET_D2C
        CloseSessionReq1    = 0x417,    // From the client
        CloseSessionReq2    = 0x418,    // Probably from the device?
        CloseSessionAck     = 0x511,    // I guess?
        KeepaliveReq        = 0x427,    // P2P_ALIVE_C2D
        KeepaliveResp       = 0x428,    // P2P_ALIVE_D2C

        // discovery
        LanSearchReq        = 0x601,    // have to send 52 bytes
        LanSearchResp       = 0x602,    // receive 184 bytes, but thats wrong

        // capability
        QueryCapabilityReq  = 0x802,    //
        QueryCapabilityResp = 0x803     //
    };

    // sent in each packet !
    constexpr std::uint16_t IOTC_MAGIC_WORD = 0x0204;

    // Port cameras listen on for LAN broadcast discovery / NAT punch requests.
    constexpr std::uint16_t IOTC_DISCOVERY_PORT = 0x7ff9;

    // converting endianness
    template <typename T>
    constexpr T to_le(T val) noexcept {
        if constexpr (std::endian::native == std::endian::big) {
            if constexpr (std::is_enum_v<T>) {
                using U = std::underlying_type_t<T>;
                return static_cast<T>(std::byteswap(static_cast<U>(val)));
            }
            else {
                return std::byteswap(val);
            }
        }
        return val;
    }

    template <typename T>
    constexpr T from_le(T val) noexcept {
        return to_le(val);
    }

    // packed to match the on-wire layout exactly
    using _PktType = IotcPacketType;
#pragma pack(push, 1)
    struct IOTCHeader
    {
        u16 magic { IOTC_MAGIC_WORD };  // +0
        u8      version     { 0 };      // +2 should be 0x06?
        u8      flags1      { 0 };      // +3 flags1
        u16     length      { 0 };      // +4
        u16     packetCount { 0 };      // +6
        _PktType type       {   };      // +8
        u16     flags2      { 0 };      // +10 flags2
        u16     rlySessionId{ 0 };      // +12
        u8      chId        { 0 };      // +14
        u8      reserved    { 0 };      // +15 unclear purpose - buf[15] in the packet handler. seems to need to be 1 to force LAN_MODE on the session.
    };
#pragma pack(pop)

    class IOTCPacket
    {
    public:
        IOTCHeader header{};
        std::vector<std::uint8_t> payload;

        IOTCPacket() = default;

        // These are the only ones that handle endianness and magic-word
        // validation, so they're the ones actually used on the wire.
        // (Used to live in IOTCPacket.cc - inlined here since this project
        // doesn't need the header/impl split for something this small.)
        [[nodiscard]] static bool deserialize(std::span<const std::uint8_t> buffer, IOTCPacket& out) {
            if (buffer.size() < sizeof(IOTCHeader))
                return false; // impossible

            std::memcpy(&out.header, buffer.data(), sizeof(IOTCHeader));

            out.header.magic = from_le(out.header.magic);
            out.header.length = from_le(out.header.length);
            out.header.packetCount = from_le(out.header.packetCount);
            out.header.type = from_le(out.header.type);
            out.header.flags1 = from_le(out.header.flags1);
            out.header.flags2 = from_le(out.header.flags2);
            out.header.rlySessionId = from_le(out.header.rlySessionId);

            if (out.header.magic != IOTC_MAGIC_WORD)
                return false; // wrong magic
            if (buffer.size() < sizeof(IOTCHeader) + out.header.length)
                return false; // wrong length

            out.payload.assign(
                buffer.begin() + sizeof(IOTCHeader),
                buffer.begin() + sizeof(IOTCHeader) + out.header.length
            );
            return true;
        }

        [[nodiscard]] static std::vector<std::uint8_t> serialize(const IOTCHeader& h, std::span<const std::uint8_t> data) {
            IOTCHeader wireHeader = h;

            wireHeader.magic = IOTC_MAGIC_WORD;
            wireHeader.length = static_cast<std::uint16_t>(data.size());

            // convert every multi-byte field to little-endian for the wire
            wireHeader.magic = to_le(wireHeader.magic);
            wireHeader.length = to_le(wireHeader.length);
            wireHeader.packetCount = to_le(wireHeader.packetCount);
            wireHeader.type = to_le(wireHeader.type);
            wireHeader.flags1 = to_le(wireHeader.flags1);
            wireHeader.flags2 = to_le(wireHeader.flags2);
            wireHeader.rlySessionId = to_le(wireHeader.rlySessionId);

            // build the payload
            std::vector<std::uint8_t> buffer(sizeof(IOTCHeader) + data.size());
            std::memcpy(buffer.data(), &wireHeader, sizeof(IOTCHeader));

            if (!data.empty()) {
                std::memcpy(buffer.data() + sizeof(IOTCHeader), data.data(), data.size());
            }

            return buffer;
        }
    };

    class IOTCPacketFactory
    {
    public:
        // TODO: could extend this to search for a specific device by uid instead
        static std::vector<u8> createLanSearch(std::string_view uid) {
            std::vector<u8> data(36, 0); // rest of the 36 bytes beyond the uid appear unused
            std::memcpy(data.data(), uid.data(), std::min(uid.size(), std::size_t(20)));

            return data;
        }

        static std::vector<u8> createNatPunch(std::string_view uid, const boost::asio::ip::address_v4& ip, u16 port) {
            std::vector<u8> data(36, 0);

            // 20 byte uid
            std::memcpy(data.data(), uid.data(), std::min(uid.size(), std::size_t(20)));

            // 2 byte AF_INET
            std::memcpy(data.data() + 20, "\x02\x00", 2);

            // 2 byte port
            std::memcpy(data.data() + 22, &port, 2);

            // 4 byte IPv4 address
            auto ipBytes = ip.to_bytes();
            std::memcpy(data.data() + 24, ipBytes.data(), 4);

            // 8 byte sin_zero, should stay zero
            return data;
        }

        // used for KNOCK and KNOCK_R
        static std::vector<u8> createKnock(std::string_view uid) {
            std::vector<u8> data(24, 0);

            // 20 byte uid
            std::memcpy(data.data(), uid.data(), std::min(uid.size(), std::size_t(20)));
            
            // 4 byte 'clientRandomId', we just hardcode this
            std::memcpy(data.data() + 20, "\xAA\xBB\xCC\xDD", 4);

            return data;
        }

    };

}