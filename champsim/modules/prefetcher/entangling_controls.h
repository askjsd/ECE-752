#ifndef ENTANGLING_CONTROLS_H
#define ENTANGLING_CONTROLS_H

#include <charconv>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

namespace entangling_controls
{
inline int
env_int(const char *name, int fallback)
{
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }

    const std::string_view text{value};
    int result{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
               ? result
               : fallback;
}

template <typename Module>
auto &
instances()
{
    static std::map<std::string, Module *, std::less<>> registered;
    return registered;
}

template <typename Module>
Module *
find(std::string_view name)
{
    const auto &registered = instances<Module>();
    auto entry = registered.find(name);
    return entry == registered.end() ? nullptr : entry->second;
}

template <typename Module>
void
unregister_instance(Module *module)
{
    auto &registered = instances<Module>();
    for (auto entry = registered.begin(); entry != registered.end(); ++entry) {
        if (entry->second == module) {
            registered.erase(entry);
            break;
        }
    }
}
} // namespace entangling_controls

#endif
