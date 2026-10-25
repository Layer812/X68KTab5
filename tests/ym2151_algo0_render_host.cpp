// Host-only playback of YM2151 VGM commands through the actual PX68K YM2151
// R61 or algorithm-0 R5-candidate FM+62.5kHz/FIR implementation.
// Compile with -DX68KTAB_YM2151_R5_ALGO0_PREVIEW=0 or =1.
// Files generated are 44.1kHz signed 16-bit little-endian stereo PCM WAV.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "../components/px68k/fmgen/vgmm5_ym2151.cpp"

static void die(const char* text) { std::fprintf(stderr,"%s\n",text); std::exit(1); }
static uint32_t le32(const std::vector<uint8_t>& d,size_t pos) {
    if (pos+4>d.size())die("Bad VGM header");
    return (uint32_t)d[pos] | (uint32_t)d[pos+1]<<8 |
        (uint32_t)d[pos+2]<<16 | (uint32_t)d[pos+3]<<24;
}
static uint16_t le16(const std::vector<uint8_t>& d,size_t pos) {
    if(pos+2>d.size())die("Bad VGM command");
    return (uint16_t)(d[pos] | (uint16_t)d[pos+1]<<8);
}
static void u16(std::ofstream& f,uint16_t value) {
    char buf[2]={(char)value,(char)(value>>8)};
    f.write(buf,2);
}
static void u32(std::ofstream& f,uint32_t value) {
    u16(f,(uint16_t)value);u16(f,(uint16_t)(value>>16));
}
int main(int argc,char**argv) {
    if(argc!=3)die("Usage: ym2151_algo0_render input.vgm output.wav");
    std::ifstream in(argv[1],std::ios::binary);
    if(!in)die("Failed to open VGM");
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),{});
    if(d.size()<0x100 || std::memcmp(d.data(),"Vgm ",4))die("Not a VGM");
    if(le32(d,0x30)!=4000000u)die("Expected 4MHz YM2151-only VGM");
    uint32_t frames=le32(d,0x18);
    if(frames==0 || frames>44100u*60u)die("Bad sample count");
    size_t offset=0x34 + le32(d,0x34);
    if(offset<0x40 || offset>=d.size())die("Bad data offset");
    VgmM5YM2151* e=vgmm5_ym2151_create(4000000,44100);
    if(!e || !e->r41_native.enabled || e->sample_rate!=62500)
        die("X68K native YM2151 62.5kHz FIR route not enabled");

    std::vector<int16_t> pcm;
    pcm.reserve((size_t)frames*2u);
    uint32_t generated=0;
    auto render_wait=[&](unsigned n) {
        if(generated+n>frames)die("Waits exceed VGM total");
        while(n) {
            const unsigned count=n>1024?1024:n;
            size_t at=pcm.size();
            pcm.resize(at+count*2u);
            vgmm5_ym2151_render(e,pcm.data()+at,count);
            generated+=count;
            n-=count;
        }
    };
    while(offset<d.size()) {
        uint8_t code=d[offset++];
        if(code==0x66)break;
        if(code==0x54) {
            if(offset+2>d.size())die("Truncated YM2151 register");
            uint8_t reg=d[offset++], value=d[offset++];
            vgmm5_ym2151_write(e,reg,value);
        } else if(code==0x61) {
            render_wait(le16(d,offset));offset+=2;
        } else if(code==0x62) {
            render_wait(735);
        } else if(code==0x63) {
            render_wait(882);
        } else if(code>=0x70 && code<=0x7f) {
            render_wait((code&15)+1);
        } else {
            die("Unexpected VGM command; this test only plays original YM2151");
        }
    }
    vgmm5_ym2151_destroy(e);
    if(generated!=frames)die("Output length != source VGM header");
    std::ofstream f(argv[2],std::ios::binary);
    if(!f)die("Failed to create WAV");
    const uint32_t bytes=frames*4u;
    f.write("RIFF",4);u32(f,36+bytes);
    f.write("WAVEfmt ",8);u32(f,16);u16(f,1);u16(f,2);
    u32(f,44100);u32(f,44100*4);u16(f,4);u16(f,16);
    f.write("data",4);u32(f,bytes);
    for(int16_t value:pcm)u16(f,(uint16_t)value);
    f.close();
    if(!f)die("Failed to write WAV");
    std::printf("PASS X68KTab5 render frames=%u, alg0 candidate=%d, output=%s\n",
        generated,X68KTAB_YM2151_R5_ALGO0_PREVIEW,argv[2]);
    return 0;
}
