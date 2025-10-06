// build: g++ -O3 -std=gnu++17 -march=native -flto run.cpp -o run
// usage:
//   Serialize:   ./run -s -i input.tsv -o graph.bin
//   Deserialize: ./run -d -i graph.bin -o output.tsv
//
// Binary format (LE, version 1):
//   [4B magic 'GRPH'][1B version=1][1B endian=1 (little)]
//   [uint32 N][uint64 M]  // N vertices; M total edges (including self-loops)
//   Section A: mapping newId->originalId: N * uint32
//   Section B: for each vertex i=0..N-1
//       deg_plus(i): VarUInt
//       then for each neighbor j>i in ascending order:
//           gap = j - prev (prev initially i), VarUInt
//           weight: 1 byte (0..255)
//   Section C (loops):
//       L: VarUInt (number of self-loops)
//       then L records:
//           vertex_delta: VarUInt (delta from the previous loop vertex, starting at 0)
//           weight: 1 byte
//
// Binary format (LE, version 2) -- backward compatible reader:
//   [4B magic 'GRPH'][1B version=2][1B endian=1 (little)]
//   [VarUInt N][VarUInt M]  // N vertices; M total edges (including self-loops)
//   Section A: mapping newId->originalId:
//       if N>0: [uint32 first_original_id]; then for i=1..N-1: delta_i = orig[i]-orig[i-1] as VarUInt
//   Section B: (same as v1)
//       for each vertex i=0..N-1:
//         deg_plus(i): VarUInt; then for each neighbor j>i:
//           gap = j - prev (prev=i), as VarUInt; weight: 1 byte
//   Section C (loops): (same as v1)
//       L: VarUInt; entries: [vertex_delta: VarUInt][weight:1B]
//
// Notes:
// - Input TSV: u \t v \t w, where u,v: uint32 and w: 0..255 (uint8); the graph is undirected.
// - During serialization each edge is stored exactly once as (min(u,v), max(u,v)).
// - During deserialization lines may be emitted in any order; here we use increasing i and neighbor order.

#include <bits/stdc++.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

using namespace std;

// ========================= Utils: die & checks =========================
[[noreturn]] static void die(const string &msg) {
    fprintf(stderr, "Error: %s\n", msg.c_str());
    exit(1);
}

static bool is_little_endian() {
    uint16_t x = 1; return *reinterpret_cast<uint8_t*>(&x) == 1;
}

// ========================= Memory-mapped file (read-only) =========================
struct MMap {
    int fd = -1;
    size_t sz = 0;
    const char* data = nullptr;
    vector<char> fallback; // if mmap unsupported, we read into buffer

    static MMap map_file(const string &path) {
        MMap m; 
        m.fd = ::open(path.c_str(), O_RDONLY);
        if (m.fd < 0) die("cannot open input: " + path);
        struct stat st{};
        if (fstat(m.fd, &st) != 0) die("fstat failed: " + path);
        m.sz = static_cast<size_t>(st.st_size);
        if (m.sz == 0) {
            // Empty file is okay; map empty region
            m.data = nullptr;
            return m;
        }
        void* p = mmap(nullptr, m.sz, PROT_READ, MAP_PRIVATE, m.fd, 0);
        if (p == MAP_FAILED) {
            // fallback: read whole file
            m.fallback.resize(m.sz);
            size_t off = 0;
            while (off < m.sz) {
                ssize_t r = ::read(m.fd, m.fallback.data() + off, m.sz - off);
                if (r < 0) die("read fallback failed: " + path);
                if (r == 0) break;
                off += (size_t)r;
            }
            if (off != m.sz) die("short read: " + path);
            m.data = m.fallback.data();
        } else {
            m.data = reinterpret_cast<const char*>(p);
        }
        return m;
    }

    void close_unmap() {
        if (data && fallback.empty() && sz > 0) {
            munmap((void*)data, sz);
        }
        if (fd >= 0) ::close(fd);
        fd = -1; data = nullptr; sz = 0; fallback.clear();
    }
    ~MMap(){ close_unmap(); }
};

// ========================= Buffered binary writer =========================
struct BinWriter {
    int fd = -1;
    vector<unsigned char> buf;
    explicit BinWriter(const string &path, size_t cap = 1<<20) {
        fd = ::open(path.c_str(), O_CREAT|O_TRUNC|O_WRONLY, 0644);
        if (fd < 0) die("cannot open output: " + path);
        buf.reserve(cap);
    }
    ~BinWriter(){ flush(); if (fd>=0) ::close(fd); }
    void flush(){ if (!buf.empty()) { ssize_t w = ::write(fd, buf.data(), buf.size()); if (w!=(ssize_t)buf.size()) die("write failed"); buf.clear(); } }
    void put(uint8_t b){ buf.push_back(b); if (buf.size()>= (1u<<20)) flush(); }
    void write(const void* p, size_t n){ const uint8_t* s=(const uint8_t*)p; for(size_t i=0;i<n;++i) put(s[i]); }
    void u32le(uint32_t x){ put((x)&0xFF); put((x>>8)&0xFF); put((x>>16)&0xFF); put((x>>24)&0xFF); }
    void u64le(uint64_t x){ for(int i=0;i<8;++i) put((x>>(8*i))&0xFF); }
    void varu(uint64_t x){ while (x>=0x80){ put(uint8_t(x)|0x80); x>>=7; } put(uint8_t(x)); }
};

// ========================= Binary reader over memory =========================
struct BinReader {
    const uint8_t* p;
    const uint8_t* e;
    explicit BinReader(const char* data, size_t sz) : p((const uint8_t*)data), e((const uint8_t*)data+sz) {}
    bool has(size_t n) const { return size_t(e-p)>=n; }
    uint8_t get(){ if(!has(1)) die("unexpected EOF in binary file"); return *p++; }
    uint32_t u32le(){ if(!has(4)) die("unexpected EOF (u32)"); uint32_t x = p[0] | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24); p+=4; return x; }
    uint64_t u64le(){ if(!has(8)) die("unexpected EOF (u64)"); uint64_t x=0; for(int i=0;i<8;++i){ x |= (uint64_t)p[i]<<(8*i); } p+=8; return x; }
    uint64_t varu(){ uint64_t x=0; int s=0; while(true){ if(!has(1)) die("unexpected EOF (varint)"); uint8_t b=*p++; x |= uint64_t(b & 0x7F) << s; if(!(b&0x80)) break; s+=7; if (s>63) die("varint too long"); } return x; }
};

// ========================= Fast TSV scanner =========================
struct TSVScanner {
    const char* p; const char* e;
    explicit TSVScanner(const char* data, size_t sz): p(data), e(data+sz) {}

    static inline void skip_newline(const char*& q, const char* e){ if(q<e && *q=='\r'){ ++q; } if(q<e && *q=='\n'){ ++q; } }

    static inline bool parse_u32_until(const char*& q, const char* e, char delim, uint32_t &out){
        uint64_t x=0; bool any=false;
        while(q<e){ unsigned char c=*q++; if (c>='0' && c<='9'){ any=true; x = x*10 + (c - '0'); if (x>0xFFFFFFFFull) return false; }
            else if (c== (unsigned char)delim){ break; }
            else if (delim=='\n' && (c=='\n' || c=='\r')){ // end of line
                // if '\r', consume possible '\n' in caller
                break;
            } else {
                // unexpected char
                return false;
            }
        }
        if(!any) return false;
        out = (uint32_t)x;
        return true;
    }

    template<class F>
    void for_each_triplet(F f){
        const char* q = p;
        while(q<e){
            // skip stray newlines
            if (*q=='\n' || *q=='\r'){ skip_newline(q,e); continue; }
            uint32_t a,b,w;
            bool ok1 = parse_u32_until(q,e,'\t', a);
            if(!ok1) die("parse error: expected first uint32 before TAB");
            bool ok2 = parse_u32_until(q,e,'\t', b);
            if(!ok2) die("parse error: expected second uint32 before TAB");
            bool ok3 = parse_u32_until(q,e,'\n', w);
            if(!ok3 || w>255) die("parse error: expected weight 0..255 then newline");
            // consume line ending if \r optional and \n
            if (q<e && q[-1]=='\r'){ // last consumed was '\r'
                if (q<e && *q=='\n') ++q; // eat '\n'
            }
            f(a,b,(uint8_t)w);
        }
    }
};

// ========================= Text writer (buffered) =========================
struct TextWriter {
    int fd = -1;
    vector<char> buf;
    explicit TextWriter(const string &path, size_t cap=1<<20){
        fd = ::open(path.c_str(), O_CREAT|O_TRUNC|O_WRONLY, 0644);
        if (fd<0) die("cannot open output: "+path);
        buf.reserve(cap);
    }
    ~TextWriter(){ flush(); if(fd>=0) ::close(fd); }
    void flush(){ if(!buf.empty()){ ssize_t w = ::write(fd, buf.data(), buf.size()); if (w!=(ssize_t)buf.size()) die("write text failed"); buf.clear(); } }
    inline void put(char c){ buf.push_back(c); if (buf.size()>= (1u<<20)) flush(); }
    inline void puts(const char* s, size_t n){ for(size_t i=0;i<n;++i) put(s[i]); }
    inline void putu(uint32_t x){ char tmp[16]; auto r = std::to_chars(tmp, tmp+16, x); if (r.ec != std::errc()) die("to_chars failed (u32)"); puts(tmp, r.ptr - tmp); }
    inline void putu8(uint8_t x){ char tmp[8]; auto r = std::to_chars(tmp, tmp+8, (unsigned)x); if (r.ec != std::errc()) die("to_chars failed (u8)"); puts(tmp, r.ptr - tmp); }
    inline void newline(){ put('\n'); }
};

// ========================= Core: serialize =========================
struct Serializer {
    string in_path, out_path;
    void run(){
        if (!is_little_endian()) die("host is not little-endian");
        MMap mm = MMap::map_file(in_path);
        const char* data = mm.data; size_t sz = mm.sz;
        TSVScanner scan(data, sz);

        // Pass 1: collect all ids
        vector<uint32_t> all_ids; all_ids.reserve(sz / 10); // heuristic
        size_t line_cnt = 0;
        scan.for_each_triplet([&](uint32_t a, uint32_t b, uint8_t w){ (void)w; all_ids.push_back(a); all_ids.push_back(b); ++line_cnt; });
        
        if (line_cnt==0){ // empty graph
            BinWriter bw(out_path);
            // header (v2)
            bw.write("GRPH",4); bw.put(2); bw.put(1); // version=2, endian
            bw.varu(0); bw.varu(0); // N, M
            // no mapping, no adj, no loops
            return;
        }

        // uniq ids -> newId mapping (sorted ascending by original id)
        vector<uint32_t> uniq = all_ids; 
        sort(uniq.begin(), uniq.end()); uniq.erase(unique(uniq.begin(), uniq.end()), uniq.end());
        all_ids.clear(); all_ids.shrink_to_fit();
        const uint32_t N = (uint32_t)uniq.size();

        auto idx_of = [&](uint32_t orig)->uint32_t{
            auto it = std::lower_bound(uniq.begin(), uniq.end(), orig);
            if (it==uniq.end() || *it!=orig) die("id not found in uniq (internal)");
            return (uint32_t)(it - uniq.begin());
        };

        // Pass 2: count deg_plus and loops
        vector<uint32_t> deg_plus(N, 0);
        uint64_t M_noLoops = 0;
        uint64_t loops_count = 0;
        TSVScanner scan2(data, sz);
        scan2.for_each_triplet([&](uint32_t a, uint32_t b, uint8_t w){
            (void)w;
            uint32_t ia = idx_of(a);
            uint32_t ib = idx_of(b);
            if (ia==ib){ ++loops_count; }
            else { uint32_t u = min(ia,ib); uint32_t v = max(ia,ib); (void)v; ++deg_plus[u]; ++M_noLoops; }
        });

        // Prefix sums for upper adjacency storage
        vector<uint64_t> off(N+1, 0);
        for (uint32_t i=0;i<N;++i) off[i+1] = off[i] + deg_plus[i];
        vector<uint32_t> upper_nei; upper_nei.resize(off[N]);
        vector<uint8_t>  upper_w;  upper_w.resize(off[N]);
        vector<uint64_t> cur = off;

        // Pass 3: fill adjacency and collect loops
        vector<pair<uint32_t,uint8_t>> loops; loops.reserve(loops_count);
        TSVScanner scan3(data, sz);
        scan3.for_each_triplet([&](uint32_t a, uint32_t b, uint8_t w){
            uint32_t ia = idx_of(a);
            uint32_t ib = idx_of(b);
            if (ia==ib){ loops.emplace_back(ia, w); }
            else {
                uint32_t u = min(ia,ib); uint32_t v = max(ia,ib);
                uint64_t pos = cur[u]++;
                upper_nei[pos] = v;
                upper_w[pos] = w;
            }
        });

        // Sort neighbor lists per vertex by neighbor (ascending), permuting weights accordingly
        vector<pair<uint32_t,uint8_t>> tmp; tmp.reserve(32);
        for (uint32_t i=0;i<N;++i){
            uint64_t b = off[i], e = off[i+1];
            size_t len = (size_t)(e-b);
            if (len<=1) continue;
            if (tmp.capacity() < len) tmp.reserve(len);
            tmp.clear();
            for (uint64_t k=b;k<e;++k) tmp.emplace_back(upper_nei[k], upper_w[k]);
            std::sort(tmp.begin(), tmp.end(), [](auto &x, auto &y){ return x.first < y.first; });
            for (size_t t=0;t<len;++t){ upper_nei[b+t] = tmp[t].first; upper_w[b+t] = tmp[t].second; }
        }

        // Sort loops by vertex ascending for delta coding
        std::sort(loops.begin(), loops.end(), [](auto &x, auto &y){ return x.first < y.first; });

        // Write binary file
        BinWriter bw(out_path);
        // header (v2)
        bw.write("GRPH",4); bw.put(2); bw.put(1); // version=2, little-endian
        bw.varu(N);
        uint64_t M_total = M_noLoops + loops.size();
        bw.varu(M_total);

        // mapping newId->originalId (delta + VarUInt)
        if (N>0){
            bw.u32le(uniq[0]);
            for (uint32_t i=1;i<N;++i){
                uint32_t d = uniq[i] - uniq[i-1];
                bw.varu(d);
            }
        }

        // upper adjacency lists with varints and 1B weights
        for (uint32_t i=0;i<N;++i){
            uint64_t b = off[i], e = off[i+1];
            uint64_t deg = e - b;
            bw.varu(deg);
            uint32_t prev = i; // delta base is current vertex index
            for (uint64_t k=b; k<e; ++k){
                uint32_t j = upper_nei[k];
                uint32_t gap = j - prev; // j>prev
                bw.varu(gap);
                bw.put(upper_w[k]);
                prev = j;
            }
        }

        // loops section
        bw.varu((uint64_t)loops.size());
        uint32_t prevLoop = 0;
        for (auto &lw : loops){
            uint32_t v = lw.first; uint8_t w = lw.second;
            uint32_t delta = v - prevLoop;
            bw.varu(delta);
            bw.put(w);
            prevLoop = v;
        }

        bw.flush();
    }
};

// ========================= Core: deserialize =========================
struct Deserializer {
    string in_path, out_path;
    void run(){
        if (!is_little_endian()) die("host is not little-endian");
        MMap mm = MMap::map_file(in_path);
        const char* data = mm.data; size_t sz = mm.sz;
        if (sz < 4+1+1+4+8) die("binary too small");
        BinReader br(data, sz);
        // header
        if (!(br.has(4))) die("no magic");
        if (br.get()!='G' || br.get()!='R' || br.get()!='P' || br.get()!='H') die("bad magic, expected 'GRPH'");
        uint8_t version = br.get(); if (version!=1 && version!=2) die("unsupported version");
        uint8_t endian = br.get(); if (endian!=1) die("unsupported endianness (only little-endian=1)");
        uint32_t N = 0;
        uint64_t M_total = 0;
        if (version==1){
            N = br.u32le();
            M_total = br.u64le(); (void)M_total;
        } else {
            N = (uint32_t)br.varu();
            M_total = br.varu(); (void)M_total;
        }

        // mapping
        vector<uint32_t> orig_of(N);
        if (version==1){
            for (uint32_t i=0;i<N;++i) orig_of[i] = br.u32le();
        } else {
            if (N>0){
                uint32_t first = br.u32le();
                orig_of[0] = first;
                for (uint32_t i=1;i<N;++i){
                    uint64_t d = br.varu();
                    orig_of[i] = orig_of[i-1] + (uint32_t)d;
                }
            }
        }

        // output TSV
        TextWriter tw(out_path);

        // adjacency
        for (uint32_t i=0;i<N;++i){
            uint64_t deg = br.varu();
            uint32_t prev = i;
            for (uint64_t k=0;k<deg;++k){
                uint64_t gap = br.varu();
                uint32_t j = prev + (uint32_t)gap;
                uint8_t w = br.get();
                // print line: orig[i] \t orig[j] \t w\n
                tw.putu(orig_of[i]); tw.put('\t');
                tw.putu(orig_of[j]); tw.put('\t');
                tw.putu8(w); tw.newline();
                prev = j;
            }
        }

        // loops
        uint64_t L = br.varu();
        uint32_t acc = 0;
        for (uint64_t t=0;t<L;++t){
            uint64_t d = br.varu();
            uint32_t v = acc + (uint32_t)d;
            uint8_t w = br.get();
            tw.putu(orig_of[v]); tw.put('\t');
            tw.putu(orig_of[v]); tw.put('\t');
            tw.putu8(w); tw.newline();
            acc = v;
        }
        tw.flush();
    }
};

// ========================= CLI =========================
bool file_exists(const string &p){ struct stat st{}; return ::stat(p.c_str(), &st)==0; }

int main(int argc, char** argv){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    if (argc<6){
        fprintf(stderr, "Usage: %s -s|-d -i <input> -o <output>\n", argv[0]);
        return 1;
    }
    bool mode_s=false, mode_d=false; string in_path, out_path;
    for (int i=1;i<argc;i++){
        string a = argv[i];
        if (a=="-s") mode_s=true; else if (a=="-d") mode_d=true;
        else if (a=="-i" && i+1<argc) { in_path = argv[++i]; }
        else if (a=="-o" && i+1<argc) { out_path = argv[++i]; }
        else { fprintf(stderr, "Unknown/invalid arg: %s\n", a.c_str()); return 1; }
    }
    if (mode_s == mode_d) die("choose exactly one mode: -s or -d");
    if (in_path.empty() || out_path.empty()) die("-i and -o are required");

    if (mode_s){
        if (!file_exists(in_path)) die("input TSV not found: "+in_path);
        Serializer s; s.in_path=in_path; s.out_path=out_path; s.run();
    } else {
        if (!file_exists(in_path)) die("input BIN not found: "+in_path);
        Deserializer d; d.in_path=in_path; d.out_path=out_path; d.run();
    }
    return 0;
}
