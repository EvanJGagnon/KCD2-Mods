// brewqueuetest.cpp -- offline tests for the AutoBrew 2.0 queue model (src/brew_queue.h).
// Build: cl /nologo /EHsc /std:c++17 /Isrc tools\brewqueuetest.cpp   or   g++ -std=c++17 -Isrc tools/brewqueuetest.cpp
// Prints ALL PASSED and exits 0, or names the failed check and exits 1.
#include "brew_queue.h"
#include <cstdio>
#include <cstring>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static bq::Key K(uint64_t n) { return {n, n * 31 + 7}; }
static bq::Ingredient I(uint64_t c, const char* name, int q, uint64_t dried = 0) { return {K(c), dried ? K(dried) : bq::Key{}, name, q}; }

int main() {
    using namespace bq;
    // Ingredient classes: 1 Nettle (dried 11), 2 Mint, 3 Sage (dried 13), 13 Dried Sage, 4 Coal
    Recipe bowman{100, "Bowman's Brew", {I(1, "Nettle", 2, 11), I(2, "Mint", 1)}};
    Recipe buck{200, "Buck's Blood", {I(1, "Nettle", 3, 11), I(4, "Coal", 1)}};
    Recipe sageA{300, "Sage Draught", {I(3, "Sage", 2, 13)}};
    Recipe sageB{400, "Dried Sage Tonic", {I(13, "Dried Sage", 1)}};

    {   // 1. one recipe: + until the ingredients run out
        Queue q; q.set_recipes({bowman}); Stock s; s.set(K(1), 4); s.set(K(2), 5);
        CHECK(q.add(s, 0) == Add::Added); CHECK(q.add(s, 0) == Add::Added);
        std::string why; CHECK(q.add(s, 0, &why) == Add::Short); CHECK(why.find("Nettle") != std::string::npos);
        CHECK(q.total() == 2); CHECK(q.add(s, 5) == Add::NoRow);
    }
    {   // 2. shared ingredient across two recipes is never promised twice
        Queue q; q.set_recipes({bowman, buck}); Stock s; s.set(K(1), 5); s.set(K(2), 9); s.set(K(4), 9);
        CHECK(q.add(s, 0) == Add::Added);              // nettle 2 of 5
        CHECK(q.add(s, 1) == Add::Added);              // nettle 5 of 5
        CHECK(q.add(s, 0) == Add::Short);              // would be 7
        CHECK(q.add(s, 1) == Add::Short);              // would be 8
        auto n = q.needs(s); CHECK(n.size() == 3); CHECK(n[0].name == "Nettle" && n[0].need == 5 && n[0].have == 5);
    }
    {   // 3. dried herbs count for the fresh line (the table substitutes them)
        Queue q; q.set_recipes({bowman}); Stock s; s.set(K(1), 1); s.set(K(11), 3); s.set(K(2), 2);
        CHECK(q.add(s, 0) == Add::Added);                        // 2 nettle from 1 fresh + 3 dried
        Stock s2 = s; s2.set(K(2), 1); Queue q2; q2.set_recipes({bowman}); q2.add(s2, 0); CHECK(q2.add(s2, 0) == Add::Short);
    }
    {   // 3b. 4 nettle usable (1 fresh + 3 dried), mint 2: two Bowman's Brews fit exactly
        Queue q; q.set_recipes({bowman}); Stock s; s.set(K(1), 1); s.set(K(11), 3); s.set(K(2), 2);
        CHECK(q.add(s, 0) == Add::Added);
        // second brew needs nettle 4 (usable 4) and mint 2 (have 2)
        CHECK(q.add(s, 0) == Add::Added);
        CHECK(q.add(s, 0) == Add::Short);
    }
    {   // 4. pools: a fresh-herb line and a dried-herb line share the dried jar
        Queue q; q.set_recipes({sageA, sageB}); Stock s; s.set(K(3), 0); s.set(K(13), 2);
        CHECK(q.add(s, 0) == Add::Added);              // sage 2 from 2 dried
        std::string why; CHECK(q.add(s, 1, &why) == Add::Short);   // dried sage 1 of 2 alone, but the pool needs 3
        CHECK(why.find("shared") != std::string::npos);
        Stock t; t.set(K(3), 1); t.set(K(13), 2);
        Queue r; r.set_recipes({sageA, sageB});
        CHECK(r.add(t, 0) == Add::Added); CHECK(r.add(t, 1) == Add::Added);   // pool 3 of 3
        CHECK(r.add(t, 1) == Add::Short); CHECK(r.add(t, 0) == Add::Short);
    }
    {   // 5. brew order = order first queued; removing to zero drops the row
        Queue q; q.set_recipes({bowman, buck}); Stock s; s.set(K(1), 99); s.set(K(2), 99); s.set(K(4), 99);
        q.add(s, 1); q.add(s, 0); q.add(s, 1);
        auto seq = q.sequence(); CHECK(seq.size() == 3); CHECK(seq[0] == 200 && seq[1] == 200 && seq[2] == 100);
        CHECK(q.summary() == "Buck's Blood x2, Bowman's Brew x1");
        CHECK(q.remove(0)); CHECK(!q.remove(0)); CHECK(q.order.size() == 1); CHECK(q.sequence().size() == 2);
        q.clear(); CHECK(q.empty()); CHECK(q.sequence().empty());
    }
    {   // 6. trim after the stock went down
        Queue q; q.set_recipes({bowman, buck}); Stock s; s.set(K(1), 10); s.set(K(2), 9); s.set(K(4), 9);
        q.add(s, 0); q.add(s, 1); q.add(s, 0);    // nettle 2+3+2 = 7
        s.set(K(1), 5); CHECK(!q.feasible(s));
        int cut = q.trim(s); CHECK(cut == 1); CHECK(q.feasible(s)); CHECK(q.total() == 2);
    }
    {   // 7. rebuilding the menu keeps the amounts and the cursor
        Queue q; q.set_recipes({bowman, buck}); Stock s; s.set(K(1), 99); s.set(K(2), 99); s.set(K(4), 99);
        q.add(s, 1); q.add(s, 1); q.cursor = 1;
        q.set_recipes({sageA, buck, bowman});
        CHECK(q.qty[1] == 2); CHECK(q.recipes[q.cursor].id == 200); CHECK(q.order.size() == 1 && q.order[0] == 1);
    }
    {   // 8. menu text: cursor, amounts, per-brew needs, shortages
        Queue q; q.set_recipes({bowman, buck}); Stock s; s.set(K(1), 5); s.set(K(2), 1); s.set(K(4), 1);
        q.add(s, 0);
        auto L = q.menu(s, 8, "help", "");
        bool cur = false, queue = false, per = false;
        for (auto& l : L) {
            if (l.text.find("> Bowman's Brew") == 0 && l.text.find("x1") != std::string::npos) cur = true;
            if (l.text.find("Queue: Bowman's Brew x1 (1 brew)") == 0) queue = true;
            if (l.text.find("Bowman's Brew (one brew): Nettle 2/5, Mint 1/1") == 0) per = true;
        }
        CHECK(cur); CHECK(queue); CHECK(per);
        s.set(K(2), 0);
        bool shortShown = false; for (auto& l : q.menu(s, 8, "help", "")) if (l.text.find("Mint 1/0 SHORT 1") != std::string::npos && l.tone == 2) shortShown = true;
        CHECK(shortShown);
        Queue e; CHECK(e.menu(s, 8, "help", "")[2].tone == 2);   // empty list says so
    }
    {   // 9. cursor wraps
        Queue q; q.set_recipes({bowman, buck, sageA}); q.move(-1); CHECK(q.cursor == 2); q.move(1); CHECK(q.cursor == 0);
        Queue e; e.move(1); CHECK(e.cursor == 0);
    }
    if (fails) { printf("%d of %d checks failed\n", fails, checks); return 1; }
    printf("brewqueuetest: %d checks, ALL PASSED\n", checks);
    return 0;
}
