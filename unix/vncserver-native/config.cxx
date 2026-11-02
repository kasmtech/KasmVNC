// SPDX-License-Identifier: GPL-2.0-or-later
// Native port of unix/vncserver and KasmVNC modules. Original launcher:
// Copyright 1999 AT&T; 2002-2005 RealVNC; 2002-2003 Constantin Kaplinsky;
// 2005-2006 Sun Microsystems; 2009-2010 D. R. Commander.
#include "config.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <regex>
#include <stdexcept>
#include <yaml.h>

extern char **environ;
namespace launcher {
#include "settings.inc"
#include "timezone-links.inc"
    namespace {
        constexpr std::size_t maxConfigBytes = 4 * 1024 * 1024;
        constexpr const char *envGate = "server.allow_environment_variables_to_override_config_settings";
        const Node absent;
        std::vector<std::string> parts(const std::string &name) {
            std::vector<std::string> result;
            std::size_t begin = 0, end;
            while ((end = name.find('.', begin)) != std::string::npos) {
                result.push_back(name.substr(begin, end - begin));
                begin = end + 1;
            }
            result.push_back(name.substr(begin));
            return result;
        }
        std::set<std::string> knownKeys() {
            std::set<std::string> result{envGate, "command_line.prompt"};
            for (const auto &option: options())
                for (const auto &key: option.keys)
                    result.insert(key.name);
            return result;
        }
        struct Parser {
            yaml_parser_t value{};
            std::string_view source;
            Parser() {
                if (!yaml_parser_initialize(&value))
                    throw std::bad_alloc();
            }
            ~Parser() {
                yaml_parser_delete(&value);
            }
            Parser(const Parser &) = delete;
            Parser &operator=(const Parser &) = delete;
        };
        struct Event {
            yaml_event_t value{};
            explicit Event(Parser &parser) {
                if (!yaml_parser_parse(&parser.value, &value))
                    throw std::runtime_error(
                        "Invalid YAML: " + std::string(parser.value.problem ? parser.value.problem : "parse error"));
            }
            ~Event() {
                yaml_event_delete(&value);
            }
            Event(const Event &) = delete;
            Event &operator=(const Event &) = delete;
        };
        Node readNode(Parser &parser, Event &event, unsigned depth) {
            if (depth > 64)
                throw std::runtime_error("YAML nesting exceeds 64 levels");
            Node out;
            switch (event.value.type) {
                case YAML_SCALAR_EVENT:
                {
                    const auto &s = event.value.data.scalar;
                    if (s.anchor || s.tag)
                        throw std::runtime_error("YAML tags and anchors are not supported");
                    std::string text(reinterpret_cast<const char *>(s.value), s.length);
                    if (text.find('\0') != std::string::npos)
                        throw std::runtime_error("NUL in configuration value");
                    if (s.style == YAML_PLAIN_SCALAR_STYLE && (text.empty() || text == "~"))
                        return out;
                    return Node(std::move(text));
                }
                case YAML_SEQUENCE_START_EVENT:
                    if (event.value.data.sequence_start.anchor || event.value.data.sequence_start.tag)
                        throw std::runtime_error("YAML tags and anchors are not supported");
                    out.kind = Node::Kind::Sequence;
                    for (;;) {
                        Event child(parser);
                        if (child.value.type == YAML_SEQUENCE_END_EVENT) {
                            if (event.value.data.sequence_start.style == YAML_FLOW_SEQUENCE_STYLE &&
                                !out.sequence.empty())
                                return Node(std::string(parser.source.substr(event.value.start_mark.index,
                                    child.value.end_mark.index - event.value.start_mark.index)));
                            break;
                        }
                        out.sequence.push_back(readNode(parser, child, depth + 1));
                    }
                    return out;
                case YAML_MAPPING_START_EVENT:
                    if (event.value.data.mapping_start.anchor || event.value.data.mapping_start.tag)
                        throw std::runtime_error("YAML tags and anchors are not supported");
                    out.kind = Node::Kind::Map;
                    for (;;) {
                        Event key(parser);
                        if (key.value.type == YAML_MAPPING_END_EVENT) {
                            if (event.value.data.mapping_start.style == YAML_FLOW_MAPPING_STYLE && !out.map.empty())
                                throw std::runtime_error("YAML flow mappings are not supported");
                            break;
                        }
                        if (key.value.type != YAML_SCALAR_EVENT)
                            throw std::runtime_error("YAML mapping key must be text");
                        std::string name(
                            reinterpret_cast<const char *>(key.value.data.scalar.value), key.value.data.scalar.length);
                        Event value(parser);
                        if (out.map.contains(name))
                            std::cerr << "Duplicate YAML key: " << name << " (last value wins)\n";
                        out.map[name] = readNode(parser, value, depth + 1);
                    }
                    return out;
                default:
                    throw std::runtime_error("Unsupported YAML node");
            }
        }
        Node readYaml(const std::string &file) {
            std::ifstream stream(file, std::ios::binary);
            if (!stream)
                throw std::runtime_error("Couldn't load config: " + file);
            std::string contents;
            char buffer[8192];
            while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) {
                contents.append(buffer, static_cast<std::size_t>(stream.gcount()));
                if (contents.size() > maxConfigBytes)
                    throw std::runtime_error("Configuration exceeds 4 MiB: " + file);
            }
            Parser parser;
            parser.source = contents;
            yaml_parser_set_input_string(
                &parser.value, reinterpret_cast<const unsigned char *>(contents.data()), contents.size());
            Event start(parser);
            Event doc(parser);
            if (doc.value.type == YAML_STREAM_END_EVENT) {
                Node empty;
                empty.kind = Node::Kind::Map;
                return empty;
            }
            Event value(parser);
            Node root = readNode(parser, value, 0);
            if (!root.present())
                root.kind = Node::Kind::Map;
            if (root.kind != Node::Kind::Map)
                throw std::runtime_error("Config must be a YAML mapping: " + file);
            // YAML::Tiny's launcher reads only document zero.
            return root;
        }
        void merge(Node &into, const Node &other) {
            if (into.kind == Node::Kind::Map && other.kind == Node::Kind::Map) {
                for (const auto &[key, value]: other.map)
                    merge(into.map[key], value);
            } else
                into = other;
        }
        void trimSections(Node &node, const std::string &path, const std::set<std::string> &keys) {
            if (node.kind != Node::Kind::Map)
                return;
            for (auto it = node.map.begin(); it != node.map.end();) {
                const std::string child = path.empty() ? it->first : path + "." + it->first;
                trimSections(it->second, child, keys);
                bool section = std::any_of(
                    keys.begin(), keys.end(), [&](const auto &key) { return key.starts_with(child + "."); });
                if (section &&
                    (!it->second.present() || (it->second.kind == Node::Kind::Map && it->second.map.empty())))
                    it = node.map.erase(it);
                else
                    ++it;
            }
        }
        void leafKeys(const Node &n, const std::string &path, std::vector<std::string> &out) {
            if (n.kind == Node::Kind::Map)
                for (const auto &[k, v]: n.map)
                    leafKeys(v, path.empty() ? k : path + "." + k, out);
            else
                out.push_back(path);
        }
        bool matches(const std::string &text, const std::string &pattern) {
            return std::regex_search(text, std::regex(pattern));
        }
        std::string scalar(const Config &config, const std::string &key) {
            return join(config.get(key).values(), ",");
        }
        bool validIp(std::string value) {
            if (value == "auto")
                return true;
            unsigned char address[16];
            // Preserve the Perl launcher's decimal IPv4 grammar, including
            // leading zeroes and its optional final dot (not inet_pton's grammar).
            static const std::regex ipv4(R"(^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)(\.|$)){4}$)");
            if (std::regex_match(value, ipv4))
                return true;
            auto scope = value.find('%');
            if (scope != std::string::npos)
                value.resize(scope);
            return inet_pton(AF_INET6, value.c_str(), address) == 1;
        }
        std::string errorFor(const Config &c, const Key &k, const std::string &value) {
            if (k.type == "INT" && !matches(value, "^-?[0-9]+$"))
                return "must be an integer";
            if (k.type == "BOOLEAN" && !matches(value, "^(true|false)$"))
                return "must be true or false";
            if (!k.allowed.empty() && std::find(k.allowed.begin(), k.allowed.end(), value) == k.allowed.end())
                return "must be one of [" + join(k.allowed, ", ") + "]";
            if (k.pattern == "@ip") {
                if (!validIp(value))
                    return k.error;
            } else if (k.pattern == "@keys") {
                bool found = false;
                for (const auto &option: options())
                    for (const auto &key: option.keys)
                        found |= key.name == value;
                if (!found) {
                    std::vector<std::string> names;
                    for (const auto &option: options())
                        for (const auto &key: option.keys)
                            names.push_back(key.name);
                    return "must be one of [" + join(names, ", ") + "]";
                }
            } else if (!k.pattern.empty() && !matches(value, k.pattern))
                return k.error;
            if (k.name == "data_loss_prevention.watermark.text.template" &&
                c.get("data_loss_prevention.watermark.image").present())
                return k.error;
            if (k.name == "data_loss_prevention.watermark.text.font_size" ||
                k.name == "data_loss_prevention.watermark.text.angle") {
                bool size = k.name.ends_with("font_size");
                if (!matches(value, size ? "^[0-9]+$" : "^-?[0-9]+$"))
                    return k.error;
                try {
                    auto number = std::stoll(value);
                    if (number < (size ? 8 : -359) || number > (size ? 256 : 359))
                        return k.error;
                } catch (...) {
                    return k.error;
                }
            }
            if (k.name == "data_loss_prevention.watermark.text.timezone_name") {
                try {
                    timezoneOffset(value);
                } catch (...) {
                    return k.error;
                }
            }
            if (k.name == "encoding.scrolling.detect_horizontal_scrolling" && value != "false" &&
                scalar(c, "encoding.scrolling.detect_vertical_scrolling") != "true")
                return k.error;
            return {};
        }
    } // namespace

    std::vector<std::string> Node::values() const {
        if (kind == Kind::Null)
            return {};
        if (kind == Kind::Scalar)
            return {text};
        if (kind != Kind::Sequence)
            throw std::runtime_error("Expected scalar or list, got mapping");
        std::vector<std::string> result;
        for (const auto &item: sequence) {
            if (item.kind == Kind::Sequence)
                throw std::runtime_error("Nested lists are not valid setting values");
            result.push_back(item.kind == Kind::Map ? "<mapping>" : item.text);
        }
        return result;
    }
    std::string join(const std::vector<std::string> &strings, const std::string &separator) {
        std::string out;
        for (std::size_t i = 0; i < strings.size(); ++i) {
            if (i)
                out += separator;
            out += strings[i];
        }
        return out;
    }
    std::string boolean(std::string value) {
        return value == "true" ? "1" : value == "false" ? "0" : value;
    }
    std::string interpolate(std::string text) {
        const std::regex pattern(R"(\$\{(\w+)\})");
        std::set<std::string> seen;
        std::smatch match;
        for (unsigned i = 0; std::regex_search(text, match, pattern); ++i) {
            if (i == 256 || text.size() > maxConfigBytes || !seen.insert(text).second)
                throw std::runtime_error("Environment interpolation cycle or size limit");
            const char *value = std::getenv(match[1].str().c_str());
            text.replace(static_cast<std::size_t>(match.position()),
                static_cast<std::size_t>(match.length()),
                value ? value : "");
        }
        return text;
    }
    const Node &Config::get(const std::string &path) const {
        const Node *node = &root;
        for (const auto &key: parts(path)) {
            if (node->kind != Node::Kind::Map)
                return absent;
            auto found = node->map.find(key);
            if (found == node->map.end())
                return absent;
            node = &found->second;
        }
        return *node;
    }
    void Config::set(const std::string &path, Node value) {
        Node *node = &root;
        for (const auto &key: parts(path)) {
            node->kind = Node::Kind::Map;
            node = &node->map[key];
        }
        *node = std::move(value);
    }
    void Config::load(const std::vector<std::string> &files, const std::string &optionalFile) {
        root.kind = Node::Kind::Map;
        for (const auto &file: files) {
            if (file == optionalFile && !std::ifstream(file))
                continue;
            auto next = readYaml(file);
            trimSections(next, "", knownKeys());
            merge(root, next);
        }
    }
    void Config::environment(bool debug, bool validationOutput) {
        const auto keys = knownKeys();
        if (get(envGate).kind == Node::Kind::Scalar && get(envGate).text == "true") {
            std::map<std::string, std::string> envNames;
            for (const auto &key: keys)
                if (key != envGate) {
                    std::string name = "KVNC_" + key;
                    for (auto &c: name)
                        c = c == '.' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    envNames[name] = key;
                }
            std::vector<std::string> unsupported;
            for (char **item = environ; *item; ++item) {
                std::string entry(*item);
                auto equals = entry.find('=');
                auto name = entry.substr(0, equals);
                if (!name.starts_with("KVNC_"))
                    continue;
                auto found = envNames.find(name);
                if (found == envNames.end())
                    unsupported.push_back(name);
                else {
                    if (debug)
                        std::cerr << "Overriding " << found->second << " with " << name << "\n";
                    set(found->second, Node(entry.substr(equals + 1)));
                }
            }
            // Keep unsupported-variable errors after setting validation, like the reference.
            if (!unsupported.empty()) {
                for (const auto &key: keys)
                    if (key != envGate && get(key).kind == Node::Kind::Scalar)
                        set(key, Node(interpolate(get(key).text)));
                validate(validationOutput, false);
                if (validationOutput)
                    std::cerr << "Unsupported config env vars found:\n" << join(unsupported, "\n") << "\n\n";
                throw Exit{1};
            }
        }
        for (const auto &key: keys)
            if (key != envGate && get(key).kind == Node::Kind::Scalar)
                set(key, Node(interpolate(get(key).text)));
    }
    void Config::validate(bool output, bool includeUnknown) const {
        auto definitions = options();
        definitions.push_back({"prompt", {{"command_line.prompt", "BOOLEAN", "", "", {}}}});
        definitions.push_back({"environment", {{envGate, "BOOLEAN", "", "", {}}}});
        for (const auto &option: definitions) {
            std::vector<std::string> errors;
            auto count = std::count_if(
                option.keys.begin(), option.keys.end(), [&](const auto &key) { return get(key.name).present(); });
            if (count && count != static_cast<long>(option.keys.size())) {
                std::vector<std::string> names;
                for (const auto &k: option.keys)
                    names.push_back(k.name);
                errors.push_back(join(names, ", ") + ": either all keys or none must be present");
            }
            for (const auto &key: option.keys) {
                const auto &node = get(key.name);
                if (!node.present() || (node.kind == Node::Kind::Scalar && node.text.empty()))
                    continue;
                // Perl's type validators inspect the whole value, while enum/pattern
                // validators inspect each element. A typed setting cannot be a list.
                const bool composite = node.kind != Node::Kind::Scalar;
                auto values = node.kind == Node::Kind::Map ? std::vector<std::string>{"<mapping>"} : node.values();
                const auto displayValue = join(values, ", ");
                auto addError = [&](const std::string &error) {
                    if (!error.empty())
                        errors.push_back(key.name + " '" + displayValue + "': " + error);
                };
                Key custom = key;
                custom.type = "ANY";
                if (key.type == "INT" || key.type == "BOOLEAN") {
                    // Check only the primitive type here, without callback validation.
                    const auto &text = node.text;
                    if (composite ||
                        (key.type == "INT" ? !matches(text, "^-?[0-9]+$") : !matches(text, "^(true|false)$")))
                        addError(key.type == "INT" ? "must be an integer" : "must be true or false");
                }
                for (const auto &value: values)
                    addError(errorFor(*this, custom, value));
            }
            // Perl stringifies references here (HASH/ARRAY plus a process address).
            // Such values cannot describe a stable Xvnc argument. Reject them explicitly.
            for (const auto &key: option.keys) {
                const auto &node = get(key.name);
                if (node.kind == Node::Kind::Sequence &&
                    (std::any_of(node.sequence.begin(),
                         node.sequence.end(),
                         [](const auto &item) { return item.kind == Node::Kind::Map; }) ||
                        option.name == "Log" || option.name == "UnixRelay"))
                    errors.push_back(key.name + ": expected a scalar value, not a nested or compound list");
            }
            if (!errors.empty()) {
                if (output)
                    std::cerr << "config errors:\n" << join(errors, "\n") << "\n\n";
                throw Exit{1};
            }
        }
        if (!includeUnknown)
            return;
        auto known = knownKeys();
        std::vector<std::string> all, unknown;
        leafKeys(root, "", all);
        for (const auto &key: all)
            if (!known.contains(key))
                unknown.push_back(key);
        if (!unknown.empty()) {
            if (output)
                std::cerr << "Unsupported config keys found:\n" << join(unknown, "\n") << "\n\n";
            throw Exit{1};
        }
    }
    int timezoneOffset(const std::string &name) {
        // The launcher is single threaded. Always restore TZ before spawning helpers.
        if (name == "floating" || name == "UTC")
            return 0;
        static const std::regex offsetPattern(R"(^[+-]?([0-9]{4}([0-9]{2})?|[0-9]{2}:[0-9]{2}(:[0-9]{2})?)$)");
        if (std::regex_match(name, offsetPattern)) {
            std::string digits;
            for (unsigned char c : name)
                if (std::isdigit(c)) digits += c;
            int hours = std::stoi(digits.substr(0, 2)), minutes = std::stoi(digits.substr(2, 2));
            int seconds = digits.size() == 6 ? std::stoi(digits.substr(4, 2)) : 0;
            if (hours > 23 || minutes > 59 || seconds > 59)
                throw std::runtime_error("Invalid timezone");
            return (name[0] == '-' ? -1 : 1) * (hours * 3600 + minutes * 60 + seconds);
        }
        std::string zone = name;
        if (auto alias = timezoneLinks.find(zone); alias != timezoneLinks.end())
            zone = alias->second;
        if (zone != "local") {
            if (zone.empty() || zone.front() == '/' || zone.find("..") != std::string::npos ||
                zone.starts_with("posix/") || zone.starts_with("right/"))
                throw std::runtime_error("Invalid timezone");
            // A regular file alone is insufficient: zone.tab and leapseconds
            // are metadata, not timezones. Require the actual TZif file magic.
            std::ifstream file("/usr/share/zoneinfo/" + zone, std::ios::binary);
            char magic[4]{};
            if (!file.read(magic, sizeof magic) || std::string(magic, 4) != "TZif")
                throw std::runtime_error("Invalid timezone");
        }
        const char *old = std::getenv("TZ");
        std::optional<std::string> saved;
        if (old)
            saved = old;
        if (zone != "local")
            setenv("TZ", (":" + zone).c_str(), 1);
        tzset();
        std::time_t now = std::time(nullptr);
        std::tm result{};
        bool valid = localtime_r(&now, &result) != nullptr;
        if (saved)
            setenv("TZ", saved->c_str(), 1);
        else
            unsetenv("TZ");
        tzset();
        if (!valid)
            throw std::runtime_error("Cannot resolve timezone");
        return static_cast<int>(result.tm_gmtoff);
    }
    std::vector<std::string> arguments(const Config &c, Context &context, const std::set<std::string> &supplied) {
        std::vector<std::string> out;
        auto value = [&](const std::string &key) { return scalar(c, key); };
        auto protocol =
            c.get("network.protocol").kind == Node::Kind::Scalar ? value("network.protocol") : std::string{};
        for (const auto &option: options()) {
            const auto &name = option.name;
            if (supplied.contains("-" + name))
                continue;
            if (c.get("network.protocol").present() && protocol != "http" && name != "noWebsocket" && name != "auth" &&
                name != "desktop" && name != "fp" && name != "geometry")
                continue;
            bool present = std::any_of(
                option.keys.begin(), option.keys.end(), [&](const auto &key) { return c.get(key.name).present(); });
            const auto firstValues = c.get(option.keys.front().name).values();
            const bool firstDefined = !firstValues.empty();
            const std::string v = firstDefined ? firstValues.front() : "";
            if (name == "auth" || name == "desktop")
                present = true;
            if (name == "fp")
                present = (firstDefined && v != "auto") || !context.fontPath.empty();
            if (!present)
                continue;
            if (name == "noWebsocket" && protocol != "vnc")
                continue;
            if (name == "AlwaysShared" && v != "shared")
                continue;
            if (name == "NeverShared" && v != "exclusive")
                continue;
            if ((name == "ZlibLevel" || name == "PublicIP" || name == "udpPort" || name == "udpSize" ||
                    name == "StunServer" || name == "DLP_WatermarkFont") &&
                (v == "auto" || !firstDefined))
                continue;
            if ((name == "DetectScrolling" || name == "DetectHorizontal" || name == "hw3d" ||
                    name == "AllowOverride") &&
                v != "true")
                continue;
            std::vector<std::string> vals;
            for (const auto &key: option.keys)
                for (auto item: c.get(key.name).values())
                    vals.push_back(boolean(item));
            std::string derived = join(vals, ",");
            const std::set<std::string> firstValueOptions{"depth",
                "websocketPort",
                "IdleTimeout",
                "DLP_ClipDelay",
                "DLP_ClipSendMax",
                "DLP_ClipAcceptMax",
                "DLP_KeyRateLimit",
                "RectThreads",
                "fp",
                "MaxDisconnectionTime",
                "MaxConnectionTime",
                "MaxIdleTime",
                "auth",
                "desktop",
                "udpFullFrameFrequency"};
            if (firstValueOptions.contains(name))
                derived = v;
            if (name == "geometry" || name == "MaxVideoResolution")
                derived = value(option.keys[0].name) + "x" + value(option.keys[1].name);
            if (name == "websocketPort" && (v == "auto" || !firstDefined))
                derived = std::to_string(8443 + context.display);
            if (name == "AlwaysShared" || name == "NeverShared" || name == "noWebsocket")
                derived = "1";
            if (name == "DisconnectClients")
                derived = v == "true" ? "1" : "0";
            if (name == "UnixRelay")
                derived = value(option.keys[0].name) + ":" + value(option.keys[1].name);
            if (name == "Log")
                derived = (value(option.keys[0].name) == "all" ? "*" : value(option.keys[0].name)) + ":" +
                          (value(option.keys[1].name) == "logfile" ? "stdout" : value(option.keys[1].name)) + ":" +
                          value(option.keys[2].name);
            if (name == "DLP_Region")
                derived = value(option.keys[1].name) + "," + value(option.keys[0].name) + "," +
                          value(option.keys[2].name) + "," + value(option.keys[3].name);
            if (v == "never" && (name == "IdleTimeout" || name == "MaxDisconnectionTime" ||
                                    name == "MaxConnectionTime" || name == "MaxIdleTime"))
                derived = "0";
            if ((v == "none" && (name == "DLP_ClipDelay" || name == "udpFullFrameFrequency")) ||
                (v == "unlimited" &&
                    (name == "DLP_ClipSendMax" || name == "DLP_ClipAcceptMax" || name == "DLP_KeyRateLimit")) ||
                (v == "auto" && name == "RectThreads"))
                derived = "0";
            if (name == "VideoArea" || name == "ScrollDetectLimit")
                derived = v.substr(0, v.size() - 1);
            if (name == "PrintVideoArea")
                derived = v == "off" ? "0" : v == "info" ? "1" : v;
            if (name == "VideoScaling")
                derived = v == "nearest" ? "0" : v == "bilinear" ? "1" : v == "progressive_bilinear" ? "2" : v;
            if (name == "CompareFB")
                derived = v == "off" ? "0" : v == "always" ? "1" : v == "auto" ? "2" : v;
            if (name == "IgnoreClientSettingsKasm")
                derived = v == "true" ? "0" : v == "false" ? "1" : v;
            if (name == "fp" && (v == "auto" || !firstDefined))
                derived = context.fontPath;
            if (name == "auth" && (v == "auto" || !firstDefined))
                derived = context.authority;
            if (name == "desktop") {
                if (!firstDefined || v == "default")
                    derived = context.desktop;
                else
                    context.desktop = derived;
            }
            if (name == "DLP_WatermarkTimeOffsetMinutes") {
                if (v.empty()) {
                    std::cerr << "Invalid empty timezone name\n";
                    throw Exit{1};
                }
                double offset = timezoneOffset(v) / 60.0;
                derived = std::to_string(offset);
                while (derived.ends_with('0'))
                    derived.pop_back();
                if (derived.ends_with('.'))
                    derived.pop_back();
            }
            if (name == "AllowOverride") {
                std::vector<std::string> names;
                for (const auto &key: c.get(option.keys[1].name).values()) {
                    bool found = false;
                    for (const auto &candidate: options()) {
                        for (const auto &k: candidate.keys)
                            if (k.name == key) {
                                names.push_back(candidate.name);
                                found = true;
                                break;
                            }
                        if (found)
                            break;
                    }
                }
                derived = join(names, ",");
            }
            if (name == "http-header") {
                for (const auto &header: c.get(option.keys[0].name).values()) {
                    out.push_back("-http-header");
                    out.push_back(header);
                }
                continue;
            }
            out.push_back("-" + name);
            const bool undefinedDerived =
                !firstDefined &&
                (name == "IdleTimeout" || name == "MaxDisconnectionTime" || name == "MaxConnectionTime" ||
                    name == "MaxIdleTime" || name == "DLP_ClipDelay" || name == "DLP_ClipSendMax" ||
                    name == "DLP_ClipAcceptMax" || name == "DLP_KeyRateLimit" || name == "RectThreads" ||
                    name == "udpFullFrameFrequency" || name == "PrintVideoArea" || name == "VideoArea" ||
                    name == "ScrollDetectLimit" || name == "VideoScaling" || name == "CompareFB");
            if (name != "hw3d" && !undefinedDerived && !(name == "depth" && v.empty()))
                out.push_back(interpolate(derived));
        }
        return out;
    }
} // namespace launcher
