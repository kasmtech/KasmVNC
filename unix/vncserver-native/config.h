// SPDX-License-Identifier: GPL-2.0-or-later
// Native port of unix/vncserver and KasmVNC modules. Original launcher:
// Copyright 1999 AT&T; 2002-2005 RealVNC; 2002-2003 Constantin Kaplinsky;
// 2005-2006 Sun Microsystems; 2009-2010 D. R. Commander.
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace launcher {
    struct Node {
        enum class Kind
        {
            Null,
            Scalar,
            Sequence,
            Map
        } kind = Kind::Null;
        std::string text;
        std::vector<Node> sequence;
        std::map<std::string, Node> map;
        Node() = default;
        explicit Node(std::string s) :
            kind(Kind::Scalar),
            text(std::move(s)) {}
        bool present() const {
            return kind != Kind::Null;
        }
        std::vector<std::string> values() const;
    };
    struct Key {
        enum class Rule {
            None, IpAddress, OptionKeys, ExcludesWatermarkImage,
            FontSize, TextAngle, TimezoneName, NeedsVerticalScroll
        };
        std::string name, type, pattern, error;
        std::vector<std::string> allowed;
        Rule rule = Rule::None;
    };
    struct Option {
        std::string name;
        std::vector<Key> keys;
    };
    const std::vector<Option> &options();
    std::string interpolate(std::string text);
    std::string join(const std::vector<std::string> &strings, const std::string &separator);
    std::string boolean(std::string value);
    struct Config {
        Node root;
        const Node &get(const std::string &path) const;
        void set(const std::string &path, Node value);
        void load(const std::vector<std::string> &files, const std::string &optionalFile);
        void environment(bool debug, bool validationOutput);
        void validate(bool validationOutput, bool includeUnknown = true) const;
    };
    struct Context {
        std::string geometry, fontPath, authority, desktop;
        int display = 1;
    };
    std::vector<std::string> arguments(const Config &, Context &, const std::set<std::string> &supplied);
    int timezoneOffset(const std::string &name);
    struct Exit {
        int status;
    };
} // namespace launcher
