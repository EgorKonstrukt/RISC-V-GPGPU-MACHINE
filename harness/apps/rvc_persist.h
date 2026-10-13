// The host's part of the guest's kept folder (docs/persist.md): the addresses "persist:..."
// of a fetch are answered from a file, which is to a harness what PlayerData is to the world.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

struct PersistHost {
    static const unsigned kLimit = 100000;   // bytes a player's data may be
    std::string file;                        // --persist FILE; FILE.auto says "0" when loading at start is off

    static bool is(const std::string& address) { return address.rfind("persist:", 0) == 0; }

    bool stored(std::vector<uint8_t>& to) const {
        FILE* f = file.empty() ? nullptr : fopen(file.c_str(), "rb");
        if (!f) return false;
        uint8_t block[4096];
        size_t n;
        to.clear();
        while ((n = fread(block, 1, sizeof block, f)) > 0) to.insert(to.end(), block, block + n);
        fclose(f);
        return !to.empty();
    }

    bool automatic() const {
        FILE* f = fopen((file + ".auto").c_str(), "rb");
        int c = f ? fgetc(f) : '1';
        if (f) fclose(f);
        return c != '0';
    }

    // The status for an address; `offset` is the request's fourth word, `data` what the guest
    // left in the fetch's 256 KB, and `reply` gets the bytes to put there (none: left as it is).
    unsigned answer(const std::string& address, unsigned offset, const uint8_t* data, std::vector<uint8_t>& reply) const {
        std::string what = address.substr(8);
        reply.clear();
        if (file.empty()) return 404;
        if (what == "load" || what == "load!") {
            if (what == "load" && !automatic()) return 403;
            return stored(reply) ? 200 : 404;
        }
        if (what == "save") {
            if (offset > kLimit) return 413;
            FILE* f = fopen(file.c_str(), "wb");
            if (!f) return 500;
            bool fine = fwrite(data, 1, offset, f) == offset;
            return fclose(f) == 0 && fine ? 200 : 500;
        }
        if (what == "clear") {
            remove(file.c_str());
            return 200;
        }
        if (what == "auto=0" || what == "auto=1") {
            FILE* f = fopen((file + ".auto").c_str(), "wb");
            if (!f) return 500;
            fputc(what[5], f);
            fclose(f);
            return 200;
        }
        if (what == "state") {
            std::vector<uint8_t> blob;
            char text[96];
            stored(blob);
            int n = snprintf(text, sizeof text, "stored %u\nlimit %u\nauto %d\n", (unsigned)blob.size(), kLimit, automatic() ? 1 : 0);
            reply.assign(text, text + n);
            return 200;
        }
        return 400;
    }
};
