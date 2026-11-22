// Plus Modem: settings (kept in flash by the board code) and small text helpers shared by the core.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#define PM_NAME "Plus Modem"
#define PM_VERSION "1.0"

struct WifiNet { bool eap; std::string ssid, pass, ident, user; };
struct Alias { std::string name, target; };               // target "host:port"
struct ModelDef { std::string id, label, tag, provider, model; };

enum ProvType { P_ANTHROPIC, P_OPENAI, P_COMPAT, P_GEMINI, P_MOCK };
struct ProviderDef { const char *name; ProvType type; const char *defUrl; bool needsKey; };
extern const ProviderDef PROVIDERS[];
enum { NPROV = 6 };                                        // anthropic openai gemini openrouter local mock
int providerIndex(const std::string &name);                // -1 if unknown

enum { MAX_NETS = 8, MAX_ALIASES = 16, MAX_MODELS = 24 };

class Config {
public:
    Config() { defaults(); }
    void defaults();

    uint32_t baud;                  // the Mac port, 1200-57600
    bool flowXon;                   // honour XON/XOFF from the Mac
    bool echo, verbose, quiet;      // the Mac line's saved profile (AT&W)
    bool monitor;                   // mirror the Mac line's commands on the USB console
    std::vector<WifiNet> nets;
    std::vector<Alias> aliases;
    std::string keys[NPROV];        // API keys (never printed; see maskKey)
    std::string urls[NPROV];        // base URLs ("" = the provider's default)
    std::vector<ModelDef> models;
    bool modelsEdited;              // false: the built-in list
    std::string defaultModel;       // "" = the first usable of claude, then the list order
    std::string pin;                // the relay's PIN ("" = none)

    std::string url(int prov) const { return urls[prov].empty() ? PROVIDERS[prov].defUrl : urls[prov]; }
    bool usable(const ModelDef &m) const;
    const ModelDef *model(const std::string &id) const;    // usable models only
    const ModelDef *firstUsable() const;
    const ModelDef *defaultUsable() const;

    // text forms for flash (one record per line, fields separated by \x1f)
    std::string netsText() const;
    void parseNets(const std::string &s);
    std::string aliasesText() const;
    void parseAliases(const std::string &s);
    std::string modelsText() const;
    void parseModels(const std::string &s);
    static std::vector<ModelDef> builtinModels();
};

// ------------------------------------------------------------------------ helpers
std::string lower(const std::string &s);
std::string upper(const std::string &s);
std::string trim(const std::string &s);
bool startsWithI(const std::string &s, const char *prefix);   // case-insensitive
std::string maskKey(const std::string &k);                     // "sk-...abcd" (short keys: "****")
// Command lines that carry secrets: the index from which characters are secret, or -1.
// AT+KEY and AT+PIN after "=", AT+WIFI after the first comma (outside quotes), AT+WIFIEAP after the third.
int secretStart(const std::string &cmd);
std::string maskLine(const std::string &cmd);
// "a","b",c -> {a, b, c}. Quotes are optional; inside them \" and \\ escape. False on a broken quote.
bool splitArgs(const std::string &s, std::vector<std::string> &out);
// "host:port", "host port" or "host" (port = defPort). False if the host is empty or the port is bad.
bool splitHostPort(const std::string &s, std::string &host, uint16_t &port, uint16_t defPort);
struct Url { bool tls; std::string host; uint16_t port; std::string path; };   // path without a trailing /
bool parseUrl(const std::string &s, Url &u);
std::string fmt(const char *f, ...);
