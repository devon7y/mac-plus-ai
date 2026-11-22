#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

const ProviderDef PROVIDERS[NPROV] = {
    {"anthropic", P_ANTHROPIC, "https://api.anthropic.com", true},
    {"openai", P_OPENAI, "https://api.openai.com/v1", true},
    {"gemini", P_GEMINI, "https://generativelanguage.googleapis.com", true},
    {"openrouter", P_COMPAT, "https://openrouter.ai/api/v1", true},
    {"local", P_COMPAT, "", false},          // Ollama / LM Studio: AT+URL local=http://host:11434/v1
    {"mock", P_MOCK, "", false},             // canned replies, free: tests the line
};

int providerIndex(const std::string &name)
{
    for (int i = 0; i < NPROV; i++)
        if (lower(name) == PROVIDERS[i].name) return i;
    return -1;
}

std::vector<ModelDef> Config::builtinModels()
{
    // the model list of askai/relay/relay.example.toml (id, label, tag, provider, model)
    static const char *const T[][5] = {
        {"claude", "Claude Opus 5.5", "Claude", "anthropic", "claude-opus-5-5"},
        {"claude-sonnet", "Claude Sonnet 5.5", "Claude", "anthropic", "claude-sonnet-5-5"},
        {"claude-haiku", "Claude Haiku 5.5 (fast)", "Claude", "anthropic", "claude-haiku-5-5"},
        {"gpt", "OpenAI GPT-5", "GPT", "openai", "gpt-5"},
        {"gpt-mini", "OpenAI GPT-5 mini", "GPT", "openai", "gpt-5-mini"},
        {"gemini", "Google Gemini 2.5 Flash", "Gemini", "gemini", "gemini-2.5-flash"},
        {"llama", "Llama 3.3 70B (OpenRouter)", "Llama", "openrouter", "meta-llama/llama-3.3-70b-instruct"},
        {"local", "Local model (Ollama)", "Local", "local", "llama3.2"},
        {"mock", "Mock Model (free test)", "Mock", "mock", "mock-1"},
    };
    std::vector<ModelDef> v;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        ModelDef m = {T[i][0], T[i][1], T[i][2], T[i][3], T[i][4]};
        v.push_back(m);
    }
    return v;
}

void Config::defaults()
{
    baud = 19200;
    flowXon = false;
    echo = verbose = true;
    quiet = false;
    monitor = true;
    nets.clear();
    aliases.clear();
    for (int i = 0; i < NPROV; i++) keys[i].clear(), urls[i].clear();
    models = builtinModels();
    modelsEdited = false;
    defaultModel.clear();
    pin.clear();
}

bool Config::usable(const ModelDef &m) const
{
    int p = providerIndex(m.provider);
    if (p < 0) return false;
    if (PROVIDERS[p].type == P_MOCK) return true;
    if (url(p).empty()) return false;                     // "local" until AT+URL sets it
    return !PROVIDERS[p].needsKey || !keys[p].empty();
}

const ModelDef *Config::model(const std::string &id) const
{
    for (size_t i = 0; i < models.size(); i++)
        if (models[i].id == id && usable(models[i])) return &models[i];
    return 0;
}

const ModelDef *Config::firstUsable() const
{
    for (size_t i = 0; i < models.size(); i++)
        if (usable(models[i])) return &models[i];
    return 0;
}

const ModelDef *Config::defaultUsable() const
{
    const ModelDef *m = model(defaultModel.empty() ? "claude" : defaultModel);
    return m ? m : firstUsable();
}

// ------------------------------------------------------------------------ text forms for flash
static std::vector<std::string> fields(const std::string &line)
{
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        size_t b = line.find('\x1f', a);
        f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return f;
}

static std::vector<std::string> lines(const std::string &s)
{
    std::vector<std::string> v;
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        if (b > a) v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    return v;
}

std::string Config::netsText() const
{
    std::string s;
    for (size_t i = 0; i < nets.size(); i++) {
        const WifiNet &n = nets[i];
        s += std::string(n.eap ? "E" : "P") + '\x1f' + n.ssid + '\x1f' + n.pass + '\x1f' + n.ident + '\x1f' + n.user + '\n';
    }
    return s;
}

void Config::parseNets(const std::string &s)
{
    nets.clear();
    std::vector<std::string> ls = lines(s);
    for (size_t i = 0; i < ls.size() && nets.size() < MAX_NETS; i++) {
        std::vector<std::string> f = fields(ls[i]);
        if (f.size() != 5 || f[1].empty()) continue;
        WifiNet n = {f[0] == "E", f[1], f[2], f[3], f[4]};
        nets.push_back(n);
    }
}

std::string Config::aliasesText() const
{
    std::string s;
    for (size_t i = 0; i < aliases.size(); i++) s += aliases[i].name + '\x1f' + aliases[i].target + '\n';
    return s;
}

void Config::parseAliases(const std::string &s)
{
    aliases.clear();
    std::vector<std::string> ls = lines(s);
    for (size_t i = 0; i < ls.size() && aliases.size() < MAX_ALIASES; i++) {
        std::vector<std::string> f = fields(ls[i]);
        if (f.size() != 2 || f[0].empty()) continue;
        Alias a = {f[0], f[1]};
        aliases.push_back(a);
    }
}

std::string Config::modelsText() const
{
    std::string s;
    for (size_t i = 0; i < models.size(); i++) {
        const ModelDef &m = models[i];
        s += m.id + '\x1f' + m.label + '\x1f' + m.tag + '\x1f' + m.provider + '\x1f' + m.model + '\n';
    }
    return s;
}

void Config::parseModels(const std::string &s)
{
    std::vector<ModelDef> v;
    std::vector<std::string> ls = lines(s);
    for (size_t i = 0; i < ls.size() && v.size() < MAX_MODELS; i++) {
        std::vector<std::string> f = fields(ls[i]);
        if (f.size() != 5 || f[0].empty()) continue;
        ModelDef m = {f[0], f[1], f[2], f[3], f[4]};
        v.push_back(m);
    }
    models = v;
    modelsEdited = true;
}

// ------------------------------------------------------------------------ helpers
std::string lower(const std::string &s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] >= 'A' && r[i] <= 'Z') r[i] += 32;
    return r;
}

std::string upper(const std::string &s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); i++)
        if (r[i] >= 'a' && r[i] <= 'z') r[i] -= 32;
    return r;
}

std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

bool startsWithI(const std::string &s, const char *prefix)
{
    size_t n = 0;
    while (prefix[n]) n++;
    return s.size() >= n && upper(s.substr(0, n)) == upper(prefix);
}

std::string maskKey(const std::string &k)
{
    if (k.empty()) return "-";
    if (k.size() < 12) return "****";
    return k.substr(0, 3) + "..." + k.substr(k.size() - 4);
}

int secretStart(const std::string &cmd)
{
    std::string u = upper(cmd);
    if (u.compare(0, 2, "AT") != 0) return -1;
    size_t plus = u.find('+', 2);
    if (plus == std::string::npos) return -1;
    std::string name = u.substr(plus + 1);
    if (name.compare(0, 3, "KEY") == 0 || name.compare(0, 3, "PIN") == 0) {
        size_t eq = u.find('=', plus);
        return eq == std::string::npos ? -1 : (int)eq + 1;
    }
    int commas;
    if (name.compare(0, 7, "WIFIEAP") == 0) commas = 3;
    else if (name.compare(0, 4, "WIFI") == 0 && name.compare(0, 8, "WIFILIST") != 0 && name.compare(0, 7, "WIFIDEL") != 0) commas = 1;
    else return -1;
    bool q = false;
    for (size_t i = plus; i < cmd.size(); i++) {
        char c = cmd[i];
        if (q && c == '\\') { i++; continue; }
        if (c == '"') q = !q;
        else if (c == ',' && !q && --commas == 0) return (int)i + 1;
    }
    return -1;
}

std::string maskLine(const std::string &cmd)
{
    int k = secretStart(cmd);
    if (k < 0) return cmd;
    return cmd.substr(0, k) + std::string(cmd.size() - k, '*');
}

bool splitArgs(const std::string &s, std::vector<std::string> &out)
{
    out.clear();
    size_t i = 0, n = s.size();
    while (i < n && s[i] == ' ') i++;
    if (i == n) return true;
    for (;;) {
        std::string v;
        while (i < n && s[i] == ' ') i++;
        if (i < n && s[i] == '"') {
            i++;
            for (;;) {
                if (i >= n) return false;                 // no closing quote
                char c = s[i++];
                if (c == '\\' && i < n) { v += s[i++]; continue; }
                if (c == '"') break;
                v += c;
            }
            while (i < n && s[i] == ' ') i++;
        } else {
            while (i < n && s[i] != ',') v += s[i++];
            v = trim(v);
        }
        out.push_back(v);
        if (i >= n) return true;
        if (s[i] != ',') return false;
        i++;
    }
}

bool splitHostPort(const std::string &s0, std::string &host, uint16_t &port, uint16_t defPort)
{
    std::string s = trim(s0);
    size_t k = s.rfind(':');
    if (k == std::string::npos) k = s.rfind(' ');
    std::string p;
    if (k != std::string::npos) {
        host = trim(s.substr(0, k));
        p = trim(s.substr(k + 1));
    } else {
        host = s;
    }
    if (host.empty() || host.find(' ') != std::string::npos) return false;
    if (p.empty()) { port = defPort; return true; }
    long v = 0;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (p[i] - '0');
        if (v > 65535) return false;
    }
    if (v == 0) return false;
    port = (uint16_t)v;
    return true;
}

bool parseUrl(const std::string &s, Url &u)
{
    std::string rest;
    if (startsWithI(s, "https://")) { u.tls = true; rest = s.substr(8); }
    else if (startsWithI(s, "http://")) { u.tls = false; rest = s.substr(7); }
    else return false;
    size_t slash = rest.find('/');
    std::string hp = rest.substr(0, slash);
    u.path = slash == std::string::npos ? "" : rest.substr(slash);
    while (!u.path.empty() && u.path[u.path.size() - 1] == '/') u.path.erase(u.path.size() - 1);
    if (hp.find(' ') != std::string::npos) return false;
    return splitHostPort(hp, u.host, u.port, u.tls ? 443 : 80);
}

std::string fmt(const char *f, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}
