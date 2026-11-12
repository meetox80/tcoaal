#include "../../core/image.h"

#include "../../core/sha256.h"

namespace images {

static bool is_hashed(const std::string& name) {
    std::string n = name;
    if (!n.empty() && n[0] == '!') n = n.substr(1);
    size_t b = n.find("[BUST]");
    if (b != std::string::npos) n = n.substr(0, b);
    if (n.size() != 16) return false;
    for (char c : n) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string resolve(const std::string& folder, const std::string& name, const char* ext) {
    if (is_hashed(name)) return folder + "/" + name;
    return hash_path(folder + "/" + name + ext);
}

}
