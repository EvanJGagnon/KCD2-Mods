// brew_queue.h -- Theatrical Autobrew 2.0: the brew queue model (no game access).
// Copyright (C) 2026 Flubbermunchkin -- GNU GPL v3 (see LICENSE.txt).
//
// The recipe menu rows (Henry's learned recipes that AutoBrew can brew), the amount queued per recipe,
// the material check for the WHOLE queue (shared ingredients are never promised twice), the brew order,
// and the menu text.  alchemy.cpp fills it from the game; tools/brewqueuetest.cpp tests it offline.
//
// Ingredient pools: the table takes a herb fresh or dried (the game's own one-hop substitute), so one
// recipe line can use two inventory classes.  Lines whose classes overlap form one pool; a queue is
// accepted only if every line has enough on its own AND every pool has enough for all its lines together.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace bq {

struct Key {
    uint64_t hi = 0, lo = 0;
    bool operator==(const Key& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const Key& o) const { return !(*this == o); }
    bool null() const { return !hi && !lo; }
};

struct Ingredient { Key cls; Key dried; std::string name; int qty = 1; };
struct Recipe { uint32_t id = 0; std::string name; std::vector<Ingredient> ing; };

// What Henry carries, per inventory class (fresh and dried counted separately).
struct Stock {
    std::vector<std::pair<Key, int>> amounts;
    int get(const Key& k) const { if (k.null()) return 0; for (auto& a : amounts) if (a.first == k) return a.second; return 0; }
    void set(const Key& k, int n) { for (auto& a : amounts) if (a.first == k) { a.second = n; return; } amounts.push_back({k, n}); }
    int usable(const Ingredient& i) const { return get(i.cls) + (i.dried != i.cls ? get(i.dried) : 0); }
};

// One ingredient line of the whole queue.
struct Need { Key cls, dried; std::string name; int need = 0, have = 0; int shortBy() const { return need > have ? need - have : 0; } };

struct Line { std::string text; int tone = 0; };   // tone: 0 normal, 1 highlighted, 2 warning, 3 dim

enum class Add { Added, Short, NoRow };

class Queue {
public:
    std::vector<Recipe> recipes;   // menu rows, in display order
    std::vector<int> qty;          // amount queued per row
    std::vector<int> order;        // rows in the order they were first queued (brew order)
    int cursor = 0;

    void set_recipes(std::vector<Recipe> r) {
        // keep amounts for recipes that are still listed (the menu is rebuilt when it opens)
        std::vector<std::pair<uint32_t, int>> keep; for (int i : order) keep.push_back({recipes[i].id, qty[i]});
        uint32_t curId = cursor >= 0 && cursor < (int)recipes.size() ? recipes[cursor].id : 0;
        recipes = std::move(r); qty.assign(recipes.size(), 0); order.clear(); cursor = 0;
        for (auto& k : keep) { int i = row_of(k.first); if (i >= 0 && k.second > 0) { qty[i] = k.second; order.push_back(i); } }
        if (int i = row_of(curId); i >= 0) cursor = i;
    }
    int row_of(uint32_t id) const { for (size_t i = 0; i < recipes.size(); i++) if (recipes[i].id == id) return (int)i; return -1; }
    int total() const { int n = 0; for (int i : order) n += qty[i]; return n; }
    bool empty() const { return total() == 0; }
    void move(int d) { if (recipes.empty()) { cursor = 0; return; } int n = (int)recipes.size(); cursor = ((cursor + d) % n + n) % n; }
    void clear() { std::fill(qty.begin(), qty.end(), 0); order.clear(); }

    // Rows with an amount in Q: the queue order, then any row only Q has (a row being added).
    std::vector<int> order_for(const std::vector<int>& Q) const {
        std::vector<int> o; for (int r : order) if (Q[r] > 0) o.push_back(r);
        for (int r = 0; r < (int)Q.size(); r++) if (Q[r] > 0 && std::find(o.begin(), o.end(), r) == o.end()) o.push_back(r);
        return o;
    }
    // Whole-queue ingredient totals, merged per inventory class, in first-use order.
    std::vector<Need> needs(const Stock& s, const std::vector<int>* q = nullptr) const {
        const std::vector<int>& Q = q ? *q : qty;
        std::vector<Need> out;
        for (int r : order_for(Q)) for (auto& g : recipes[r].ing) {
            Need* n = nullptr; for (auto& e : out) if (e.cls == g.cls) { n = &e; break; }
            if (!n) { out.push_back({g.cls, g.dried, g.name, 0, s.usable(g)}); n = &out.back(); }
            n->need += g.qty * Q[r];
        }
        return out;
    }
    // Can the whole queue (amounts q) be supplied?  Fills `why` with the first shortage.
    bool feasible(const Stock& s, const std::vector<int>& q, std::string* why = nullptr) const {
        auto ns = needs(s, &q);
        for (auto& n : ns) if (n.need > n.have) { if (why) *why = n.name + " short " + std::to_string(n.need - n.have); return false; }
        // pools: lines that can draw on the same class (a fresh herb and its dried form listed on its own)
        std::vector<int> pool(ns.size()); for (size_t i = 0; i < ns.size(); i++) pool[i] = (int)i;
        auto shares = [](const Need& a, const Need& b) {
            auto has = [](const Need& n, const Key& k) { return !k.null() && (n.cls == k || n.dried == k); };
            return has(a, b.cls) || (!b.dried.null() && has(a, b.dried));
        };
        for (size_t i = 0; i < ns.size(); i++) for (size_t j = i + 1; j < ns.size(); j++)
            if (shares(ns[i], ns[j])) { int a = pool[i], b = pool[j]; for (auto& p : pool) if (p == b) p = a; }
        for (size_t i = 0; i < ns.size(); i++) {
            if (pool[i] != (int)i) continue;
            int need = 0; std::vector<Key> classes;
            for (size_t j = 0; j < ns.size(); j++) if (pool[j] == (int)i) {
                need += ns[j].need;
                for (const Key& k : {ns[j].cls, ns[j].dried}) if (!k.null() && std::find(classes.begin(), classes.end(), k) == classes.end()) classes.push_back(k);
            }
            int have = 0; for (auto& k : classes) have += s.get(k);
            if (need > have) { if (why) *why = ns[i].name + " (shared) short " + std::to_string(need - have); return false; }
        }
        return true;
    }
    bool feasible(const Stock& s, std::string* why = nullptr) const { return feasible(s, qty, why); }

    // +1 on a row, only if the whole queue can still be supplied.
    Add add(const Stock& s, int row, std::string* why = nullptr) {
        if (row < 0 || row >= (int)recipes.size()) return Add::NoRow;
        std::vector<int> q = qty; q[row]++;
        if (!feasible(s, q, why)) return Add::Short;
        qty[row]++; if (std::find(order.begin(), order.end(), row) == order.end()) order.push_back(row);
        return Add::Added;
    }
    bool remove(int row) {
        if (row < 0 || row >= (int)recipes.size() || qty[row] <= 0) return false;
        if (--qty[row] == 0) order.erase(std::remove(order.begin(), order.end(), row), order.end());
        return true;
    }
    // Lower amounts from the end of the queue until it fits what Henry has now (e.g. after he sold or used something).
    int trim(const Stock& s) {
        int cut = 0;
        while (!order.empty() && !feasible(s)) { remove(order.back()); cut++; }
        return cut;
    }

    // Recipe ids in brew order: every brew of the first queued recipe, then the next, ...
    std::vector<uint32_t> sequence() const {
        std::vector<uint32_t> s; for (int r : order) for (int k = 0; k < qty[r]; k++) s.push_back(recipes[r].id);
        return s;
    }
    std::string summary() const {   // "Bowman's Brew x2, Buck's Blood x1"
        std::string s; for (int r : order) { if (!s.empty()) s += ", "; s += recipes[r].name + " x" + std::to_string(qty[r]); }
        return s;
    }

    // The on-screen menu.  keys: {up/down, +/-, clear, start} names for the help line.
    std::vector<Line> menu(const Stock& s, int maxRows, const char* keyHelp, const std::string& status) const {
        std::vector<Line> L;
        L.push_back({"AutoBrew queue", 1});
        L.push_back({keyHelp, 3});
        if (recipes.empty()) { L.push_back({"No recipes AutoBrew can brew are known yet.", 2}); return L; }
        int n = (int)recipes.size(), rows = std::min(n, std::max(3, maxRows));
        int top = std::min(std::max(0, cursor - rows / 2), n - rows);
        if (top > 0) L.push_back({"   ...", 3});
        for (int i = top; i < top + rows; i++) {
            char b[200]; snprintf(b, sizeof b, "%s %-34s %s", i == cursor ? ">" : " ", recipes[i].name.c_str(), qty[i] ? ("x" + std::to_string(qty[i])).c_str() : "");
            L.push_back({b, i == cursor ? 1 : 0});
        }
        if (top + rows < n) L.push_back({"   ...", 3});
        const Recipe& c = recipes[cursor];
        std::string per = c.name + " (one brew):";
        for (auto& g : c.ing) { per += " " + g.name + " " + std::to_string(g.qty) + "/" + std::to_string(s.usable(g)) + ","; }
        if (!per.empty() && per.back() == ',') per.pop_back();
        L.push_back({per, 0});
        if (!empty()) {
            L.push_back({"Queue: " + summary() + " (" + std::to_string(total()) + (total() == 1 ? " brew)" : " brews)"), 1});
            L.push_back({"Ingredients for the whole queue (need / have):", 0});
            std::string row; bool warn = false;
            for (auto& x : needs(s)) {
                std::string item = x.name + " " + std::to_string(x.need) + "/" + std::to_string(x.have);
                if (x.shortBy()) { item += " SHORT " + std::to_string(x.shortBy()); warn = true; }
                if (row.size() + item.size() > 90) { L.push_back({row, warn ? 2 : 0}); row.clear(); warn = false; }
                row += (row.empty() ? "" : ", ") + item;
            }
            if (!row.empty()) L.push_back({row, warn ? 2 : 0});
        }
        if (!status.empty()) L.push_back({status, 2});
        return L;
    }
};

}  // namespace bq
