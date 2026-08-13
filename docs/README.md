# TUTK platform protocols (IOTC, AVAPI)

This contains some information about the protocols, the obfuscation ('transcode') and such

## reversing the scrambler
* Well known xor key: `Charlie is the designer of P2P!!`

basically, it works like this
* If the buffer length is not >= 16 bytes
    * Process the remaining bytes
* Repeat for all 16-byte blocks
    * Inverse final rotation
        * Rotate 32-bit words LEFT by (3, 7, 11, 15) bits
    * Mix bytes
        * block permutation via the permutation table
    * XOR 16 bytes with the key
        * easy!
    * Rotate words again
        * Rotate 32-bit words LEFT by (1, 5, 9, 13) bits
* Process the remaning tail bytes
    * Mix bytes
        * block permutation via the permutation table
    * XOR with the key

of course, the scrambling works in reverse

## IOTC header
```cpp
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
    u8      reserved    { 0 };      // +15 buf[15] in the packet handler. seems to need to be 1 to force LAN_MODE on the session.
};
```

## AVAPI header
```cpp
struct AVPacketHead
{
    AVPacketType type{};            // protocol type?
    AVOpCode opCode{};              // AVOpCode
    std::uint16_t ver{ 1 };         // version, always 1
    std::uint32_t frmNo{};          // frame sequence number
    std::uint32_t frmSize{};        // expected video data size
    std::uint16_t packNoInFrm{};    // chunk index
    std::uint16_t frmInfoSize{};    // FRAMEINFO size
    std::uint16_t payload{};        // size of body (sometimes ioCtrlBufSize+4)
    std::uint8_t reserve1[6]{};     // pad/align
};

struct AVPacket
{
    AVPacketHead protoHead;
    std::vector<std::uint8_t> body;
}
```
As an example, here is obfuscated 16-bit little endian PCM (8000KHz sample rate) data. The data itself is 800 bytes, which means we have 56 bytes of headers.
```
4e6f8d0c40d040ca3d3f682cc0e680ec7d0e8d8c40d041ca3d6d284c00e5cbd0462e8d4c40d040ca2d6d3a2c40e4cad8a60bcd8e420161ea2d6d280c40e4cad80b0f6d0d4230c0d12dd12a2510e7effcde255dec407051c42da3f014e0e5c5c89f3f8d8d4091d1c52d3bda1e00e4c4c81637fd6d407191dc3d043e1b90e5dcccdb3bdded4020f0dd3df83215c0e5d2c0fc219dad401160db3dfdff1470e4d8cc0928cdec40b0c1ca2d91250490e4cfda50d5e293bfbf6e32d2acb6eda1e5c9260f285d2c40f021c6226ab5eeeee435db26253dcc4090d0c03dc3210420e5ccdee33e5dcc402070c53d41830740e5c0d23f26fd0c40d011c22d95c501f0e5c7d28f265dcc402031c22d80e20ba0e5c8d80c317d53bf5f3e362d26400520e5c0d23adfa2b3bf1fdf3ac209d3fadf1a382dc6c41232bf5eff2dc25a38e78f1a223304c45272bf4f7e24d2a822e75f1b253726c3a293bf6ecf3bd2ce58e2df1b2c3f65de4252bfafef2dd25067e6df1b23338dce62d1bf4e0e16d222efe5cf1b2f318bcdf2b2bf2eaf22c23a49edff1a1439f9d2a24cbe1020c8d27807e22f1a3e2d72d1828cbf402f36d2ac53faff1b362f50ced2cc400191cf327f36e8d0e4ccde72d24213bf1eff322d9b0e0baee534dbd8d75293bfbf1f33c2e5d1f83f1b3e29b3d56233be2fe1cad21ef0f11f1b36253ddd62f2bf6eaf3bd285f4f46f1a3c21b0f322d1bd6ffe14c2a347e32f1b2f3597f61211bddf3f1ed2b315d5ff191e0d2bcdc252bf8e1e22d2983cd5ef192e0723c142b2bffe4f25c2adcfe0ef1a3a29fcc702f2bf2e3e23c26a7bff2f1b3937fbd3920cbf604f37d2d024e17f1b3b29a5250d2c400110c2220ef5ed40e5c1de22237d6c40d010db2d21e207b0e4c6c820271d4c40f1f1c22de8e703f0e4c1d2cc2ccdac41e01f343d75e30450e5ccd25bd9c253bfaf0f3cc207d2fb7f1b3c2115d26273be8e00cec20c70f9bf1b372dc03d0d2d40f1d0d02d9c6f0030e4dad67b38ed4d40f071c53d0ed31530e4ddc0c1270d2c404010c63d50830400e5c7d20700adcf426011e42d6cfd1f10e5e3c007082d2e425021e32dce2738b0e6e4ea2725fd2c406130c32d48501960e4c6ca5f3f8d2d406070c52d8b820010e5d8c8c5223dec40d1e0c73dc6e300e0e5c0d4e9252d6c402081c23df89a1cd0e5c7d62016720a68026917
```
Descrambled, we get
```
040205024803b20408041200000000000104010094000000300300000000100020030000000000008c00020000000000000000000000000081025c0242021d022b02900255025c0258021602f801ae018d018c011a01ee000601bb00c100fa0024012e010401f60011011e011901f400660171015d0169018f0197016d017a01a80191019801950156015b015b016f0178018d01430130012301f90012010c01df0080002d005c007600660048001600edff1900feff2100e1ffb2ff62ff86ffd1ff28002a00e7ff1a006700c600d2009a00810076006e008400b500a900a400b200bb00b000ac00d4000901d300ef00df00de00bb00d8001801850065009000ae007c001e002d0014008f00c7009f0084009600b600ab002d00f7ffe7ffc8ffb6ff6eff49ff24ff43ff14ff29ff1cff02ffb0feacfe97fe8afeabfe9bfe78feadfeb1fef1fee5feaffea7fec3fee9fe0affe7fe79fe63fe80fed4fe38ff1bfff3fea5feb9fe9dfebdfe00ffdafe77fe75fe9dfe98fe4ffe34fe0efee4fdc9fd14fe16fe0bfef7fd52fe3ffe6efe9ffef0fee3fe46ff55ff7cffc8ff26002c00bcff67ff7bffc1ffc0fff0ff0600d9ffe0ff410069007200e5ff03005d005d006e0072003e00f6ffc8ffc0ffcbff9cff98ff4eff17ff48ff61ff9bffd5ff96ff86ffddffd5ffd3ffdbffbcffeaff1f00ceff8dff82ff68ff36ff34fffefe1bfffdfef6fed6fe4efee5fddcfdabfdebfddcfd93fd5bfd5efd99fd8ffd87fd50fd4efd91fdfafd55fe5cfe34fe45fe85fe7cfecefe0aff00ffd2fef4fed0fef2fe31ff34fff6fe27ff26ff98fe97fe8ffecbfed1fe13ff1dff58ffd9ff1000dbffd7ff1c007000a300bb00bc0085008d00a400bc000f01cc00ce00d500f5000001f900fc00ab00a500ed0095008b009200b0008d00b100780022002a004500f3ffa6ff7eff93ff6aff5dff72ff44ff77ff87ff54ff5fffc1ffbeffc6ffe4ff45007e00c400e7001101eb003a018901a201b7019f01870163015c0160017301f200b3008b00b400dd00fa009a008500c90051013c01950181029402e6022503fb02fa0240032f03e3029a026602b6029802820156012201c5009b00b50077008b00ae00ca00150126011a0112011301eb00ba00cd00da00ab00b700cb00ba00c00029010b01f900d5007e00b800ac009f0054006b007a006700
```
We can see the 0x0204 magic, and such. No problem.

## more information
* You can find the SDKs on the internet, have fun looking them up. SHA256 sums
```
05463b5a35e83edc3c185b6173723ea191a4331530c97f097d74c48fed6943e7  TUTK_IOTC_Platform_14W42P1.zip
c82717bc7016968bf3d0b70277779c5a2c8b27e80da627dccdc1c393a6d48152  TUTK_IOTC_Platform_14W36.zip
```
* Original camera used an android application `uCareCam`. It seems it contains custom IOCTRL definitions, but i haven't bothered to make sense of them all