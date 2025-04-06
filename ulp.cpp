#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <regex>

std::string trim(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::string toLower(const std::string &s) {
    std::string res = s;
    std::transform(res.begin(), res.end(), res.begin(), ::tolower);
    return res;
}

std::vector<std::string> split(const std::string &s, const std::string &delimiter) {
    std::vector<std::string> tokens;
    size_t start = 0, pos;
    while ((pos = s.find(delimiter, start)) != std::string::npos) {
        tokens.push_back(s.substr(start, pos - start));
        start = pos + delimiter.length();
    }
    tokens.push_back(s.substr(start));
    return tokens;
}

struct Config {
    std::string separator;
    std::string format;
    std::string convert_format;
    std::unordered_set<std::string> email_remove;
    std::unordered_set<std::string> email_contains;
    std::unordered_set<std::string> url_remove;
    std::unordered_set<std::string> url_contains;
};

Config parseConfig(const std::string &filename) {
    Config config;
    std::ifstream file(filename);
    if (!file) std::exit(1);
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));
        if (key == "separator") config.separator = value;
        else if (key == "format") config.format = value;
        else if (key == "convert_format") config.convert_format = value;
        else {
            auto tokens = split(value, ",");
            for (auto &t : tokens) {
                t = toLower(trim(t));
                if (key == "email_remove") config.email_remove.insert(t);
                else if (key == "email_contains") config.email_contains.insert(t);
                else if (key == "url_remove") config.url_remove.insert(t);
                else if (key == "url_contains") config.url_contains.insert(t);
            }
        }
    }
    return config;
}

std::string extractEmailDomain(const std::string &email) {
    size_t pos = email.find('@');
    return (pos == std::string::npos) ? "" : toLower(email.substr(pos + 1));
}

std::string extractUrlDomain(const std::string &url) {
    std::regex re("^(?:https?://)?(?:www\\.)?([^/]+)");
    std::smatch match;
    if (std::regex_search(url, match, re) && match.size() >= 2)
        return toLower(match[1].str());
    return "";
}

bool domainMatches(const std::string &domain, const std::string &pattern) {
    if (domain == pattern) return true;
    if (domain.size() > pattern.size() &&
        domain.compare(domain.size() - pattern.size(), pattern.size(), pattern) == 0 &&
        (domain.size() == pattern.size() || domain[domain.size() - pattern.size() - 1] == '.'))
        return true;
    return false;
}

bool checkDomain(const std::string &domain, const std::unordered_set<std::string> &removeSet,
                 const std::unordered_set<std::string> &containSet) {
    for (const auto &r : removeSet)
        if (domainMatches(domain, r))
            return false;
    if (!containSet.empty()) {
        bool found = false;
        for (const auto &c : containSet)
            if (domainMatches(domain, c)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

bool isValidEmail(const std::string &s) {
    std::regex emailRegex("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}$");
    return std::regex_match(s, emailRegex);
}

bool isPhoneNumber(const std::string &s) {
    std::regex phoneRegex("^\\+?\\d{6,}$");
    return std::regex_match(s, phoneRegex);
}

int main(int argc, char* argv[]){
    if (argc < 2) return 1;
    Config config = parseConfig("config.ini");
    std::ifstream infile(argv[1]);
    std::ofstream outfile("filtered_ulp.txt");
    if (!infile || !outfile) return 1;
    std::string line;
    while (std::getline(infile, line)) {
        if (line.empty()) continue;
        auto tokens = split(line, config.separator);
        std::string url, login, pass;
        bool valid = false;
        if (config.format == "url:email:pass") {
            if (tokens.size() < 3) continue;
            url = trim(tokens[0]);
            login = trim(tokens[1]);
            pass = trim(tokens[2]);
            if (!isValidEmail(login) || isPhoneNumber(login))
                continue;
            valid = true;
        } else if (config.format == "email:pass") {
            if (tokens.size() < 2) continue;
            login = trim(tokens[0]);
            pass = trim(tokens[1]);
            if (!isValidEmail(login) || isPhoneNumber(login))
                continue;
            valid = true;
        }
        if (!valid) continue;
        std::string emailDomain = extractEmailDomain(login);
        if (!checkDomain(emailDomain, config.email_remove, config.email_contains))
            continue;
        if (!config.url_remove.empty() || !config.url_contains.empty()) {
            if (!url.empty()) {
                std::string urlDomain = extractUrlDomain(url);
                if (!checkDomain(urlDomain, config.url_remove, config.url_contains))
                    continue;
            }
        }
        if (config.convert_format == "email:pass" && config.format == "url:email:pass")
            outfile << login << config.separator << pass << "\n";
        else
            outfile << line << "\n";
    }
    return 0;
}
