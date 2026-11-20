// Standalone logic test — offline catalog tree building (T2, SPEC AC2/AC10/AC20).
//
//   c++ -std=gnu++17 -arch x86_64 \
//       -Iapp/include -Ilibrary/borealis/library/include/borealis/extern \
//       tests/test_offline_catalog.cpp -o /tmp/t && /tmp/t
//
// Exercises the PURE catalog logic (offline_catalog.hpp) that turns a flat set
// of plex::Item nodes into a navigable libraries -> shows -> seasons -> episodes
// tree, including synthesis of missing ancestors for legacy downloads.

#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <utils/offline_catalog.hpp>

static int failures = 0;
#define CHECK(cond)                                          \
    do {                                                     \
        if (!(cond)) {                                       \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                      \
        }                                                    \
    } while (0)

static plex::Item movie(const std::string& key, const std::string& title, const std::string& sec) {
    plex::Item it;
    it.ratingKey = key;
    it.type = plex::mediaTypeMovie;
    it.title = title;
    it.librarySectionID = sec;
    it.librarySectionTitle = "Films";
    return it;
}

// legacy episode: no real ancestor ratingKeys, only grandparentTitle + indexes
static plex::Item legacyEpisode(const std::string& key, const std::string& show, int season, int ep) {
    plex::Item it;
    it.ratingKey = key;
    it.type = plex::mediaTypeEpisode;
    it.title = "Ep " + std::to_string(ep);
    it.grandparentTitle = show;
    it.parentIndex = season;
    it.index = ep;
    return it;
}

int main() {
    std::vector<plex::Item> nodes;
    nodes.push_back(movie("m1", "Zodiac", "1"));
    nodes.push_back(movie("m2", "Amelie", "1"));
    // two legacy episodes of the same show/season, added out of order
    nodes.push_back(legacyEpisode("e2", "Scrubs", 1, 2));
    nodes.push_back(legacyEpisode("e1", "Scrubs", 1, 1));

    auto full = offline::synthesizeAncestors(nodes);

    // a show node and a season node must have been synthesized
    int shows = 0, seasons = 0, episodes = 0, movies = 0;
    for (auto& it : full) {
        if (it.type == plex::mediaTypeShow) ++shows;
        else if (it.type == plex::mediaTypeSeason) ++seasons;
        else if (it.type == plex::mediaTypeEpisode) ++episodes;
        else if (it.type == plex::mediaTypeMovie) ++movies;
    }
    CHECK(movies == 2);
    CHECK(shows == 1);
    CHECK(seasons == 1);
    CHECK(episodes == 2);

    // sections: one Films (movies), one for the show
    auto sections = offline::buildSections(full);
    CHECK(sections.size() == 2);

    // the movie section holds both films, sorted by title (Amelie < Zodiac)
    auto films = offline::sectionItems(full, "1");
    CHECK(films.size() == 2);
    CHECK(films.size() == 2 && films[0].title == "Amelie" && films[1].title == "Zodiac");

    // offline search (T-search): top-level items only (movies + shows),
    // case-insensitive title contains, sorted by title
    {
        // empty term -> all top-level items (offline "suggestions"): Amelie,
        // Scrubs, Zodiac (episodes excluded)
        auto all = offline::search(full, "");
        CHECK(all.size() == 3);
        CHECK(all.size() == 3 && all[0].title == "Amelie" && all[1].title == "Scrubs" && all[2].title == "Zodiac");

        // case-insensitive contains on the show title
        auto scrubs = offline::search(full, "SCR");
        CHECK(scrubs.size() == 1 && scrubs.front().title == "Scrubs");

        // matches a movie
        auto ame = offline::search(full, "ame");
        CHECK(ame.size() == 1 && ame.front().title == "Amelie");

        // episodes are NOT top-level: their title ("Ep 1") never surfaces
        CHECK(offline::search(full, "Ep 1").empty());

        // no match
        CHECK(offline::search(full, "zzz").empty());
    }

    // find the synthesized show and walk down to episodes
    std::string showKey;
    for (auto& it : full)
        if (it.type == plex::mediaTypeShow) showKey = it.ratingKey;
    CHECK(!showKey.empty());

    auto seasonsOf = offline::childrenOf(full, showKey);
    CHECK(seasonsOf.size() == 1);
    CHECK(seasonsOf.size() == 1 && seasonsOf[0].type == plex::mediaTypeSeason);

    std::string seasonKey = seasonsOf.empty() ? "" : seasonsOf[0].ratingKey;
    auto eps = offline::childrenOf(full, seasonKey);
    CHECK(eps.size() == 2);
    // episodes sorted by index (e1 before e2)
    CHECK(eps.size() == 2 && eps[0].ratingKey == "e1" && eps[1].ratingKey == "e2");

    // idempotence: real ancestor nodes already present must not be duplicated
    plex::Item show;
    show.ratingKey = "S9";
    show.type = plex::mediaTypeShow;
    show.title = "Real Show";
    plex::Item season;
    season.ratingKey = "S9-1";
    season.type = plex::mediaTypeSeason;
    season.parentRatingKey = "S9";
    season.index = 1;
    plex::Item epReal;
    epReal.ratingKey = "E100";
    epReal.type = plex::mediaTypeEpisode;
    epReal.parentRatingKey = "S9-1";
    epReal.grandparentRatingKey = "S9";
    epReal.index = 1;
    std::vector<plex::Item> real = {show, season, epReal};
    auto real2 = offline::synthesizeAncestors(real);
    CHECK(real2.size() == 3);  // nothing synthesized
    CHECK(offline::childrenOf(real2, "S9").size() == 1);
    CHECK(offline::childrenOf(real2, "S9-1").size() == 1);

    // asset paths to cache for a fiche (T3): non-empty images only
    plex::Item art;
    art.thumb = "/t";
    art.art = "/a";
    art.clearLogo = "/l";
    plex::Role withFace;
    withFace.thumb = "/r1";
    plex::Role noFace;  // empty thumb must be skipped
    art.roles = {withFace, noFace};
    auto assets = offline::assetPaths(art);
    CHECK(assets.size() == 4);

    // prune survivors (T4): given which leaves are downloaded, decide what to keep
    {
        auto mk = [](const std::string& key, const std::string& type, const std::string& parent,
                      const std::string& grand) {
            plex::Item it;
            it.ratingKey = key;
            it.type = type;
            it.parentRatingKey = parent;
            it.grandparentRatingKey = grand;
            return it;
        };
        std::vector<plex::Item> pn;
        pn.push_back(mk("m1", plex::mediaTypeMovie, "", ""));      // downloaded
        pn.push_back(mk("m2", plex::mediaTypeMovie, "", ""));      // NOT downloaded
        pn.push_back(mk("S", plex::mediaTypeShow, "", ""));
        pn.push_back(mk("S1", plex::mediaTypeSeason, "S", ""));    // has a download
        pn.push_back(mk("S2", plex::mediaTypeSeason, "S", ""));    // no download
        pn.push_back(mk("e1", plex::mediaTypeEpisode, "S1", "S")); // downloaded
        pn.push_back(mk("e2", plex::mediaTypeEpisode, "S1", "S")); // sibling, not downloaded
        pn.push_back(mk("e3", plex::mediaTypeEpisode, "S2", "S")); // not downloaded

        std::unordered_set<std::string> dl = {"m1", "e1"};
        auto keep = offline::survivors(pn, [&](const std::string& k) { return dl.count(k) > 0; });

        CHECK(keep.count("m1") == 1);
        CHECK(keep.count("m2") == 0);  // movie not downloaded -> pruned
        CHECK(keep.count("S") == 1);   // show has a downloaded episode
        CHECK(keep.count("S1") == 1);  // season with a download
        CHECK(keep.count("e1") == 1);
        CHECK(keep.count("e2") == 1);  // greyed sibling kept (season has a download)
        CHECK(keep.count("S2") == 0);  // season with no download -> pruned
        CHECK(keep.count("e3") == 0);  // its episode -> pruned
    }

    {
        // the Emby media source id survives the offline snapshot
        media::Media m;
        m.sourceId = "mediasource_743";
        m.width = 1920;
        nlohmann::json j = m;
        media::Media back = j.get<media::Media>();
        CHECK(back.sourceId == "mediasource_743");
        CHECK(back.width == 1920);
    }

    {
        // capture of an Emby episode: the server's own shape (episode 747 under
        // season 745 under series 744, library 5 "Series"), no librarySectionID
        // in the metadata
        std::map<std::string, plex::Item> server;
        auto node = [&](const std::string& key, const std::string& type, const std::string& parent,
                        const std::string& grandparent, int64_t index) {
            plex::Item it;
            it.ratingKey = key;
            it.type = type;
            it.title = type + " " + key;
            it.parentRatingKey = parent;
            it.grandparentRatingKey = grandparent;
            it.index = index;
            it.parentIndex = type == plex::mediaTypeEpisode ? 1 : 0;
            server[key] = it;
        };
        node("744", plex::mediaTypeShow, "4", "", 0);
        node("745", plex::mediaTypeSeason, "744", "744", 1);
        node("746", plex::mediaTypeEpisode, "745", "744", 2);
        node("747", plex::mediaTypeEpisode, "745", "744", 1);
        node("743", plex::mediaTypeMovie, "742", "", 0);
        server["747"].summary = "full fiche";
        plex::Section series;
        series.key = "5";
        series.title = "Series";

        std::vector<plex::Item> stored;
        std::vector<std::string> withArt;
        std::set<std::string> failing;
        auto fetch = [&](const std::string& id) -> std::optional<plex::Item> {
            if (failing.count(id)) throw std::runtime_error("unreachable");
            auto it = server.find(id);
            if (it == server.end()) return std::nullopt;
            return it->second;
        };
        auto children = [&](const std::string& id) {
            std::vector<plex::Item> out;
            for (auto& [k, it] : server)
                if (it.parentRatingKey == id) {
                    plex::Item ep = it;
                    ep.summary.clear();  // list endpoints return a lighter shape
                    out.push_back(ep);
                }
            return out;
        };
        auto has = [&](const std::string& id) {
            for (auto& it : stored)
                if (it.ratingKey == id) return true;
            return false;
        };
        auto put = [&](plex::Item it, bool art) {
            if (art) withArt.push_back(it.ratingKey);
            stored.push_back(std::move(it));
        };

        offline::capture("747", series, fetch, children, has, put);

        CHECK(stored.size() == 4);  // leaf, show, season, one sibling
        CHECK(stored.size() > 0 && stored[0].ratingKey == "747");
        CHECK(stored.size() > 0 && stored[0].summary == "full fiche");  // leaf not overwritten by the list
        CHECK((withArt == std::vector<std::string>{"747", "744", "745"}));
        for (auto& it : stored) CHECK(it.librarySectionID == "5" && it.librarySectionTitle == "Series");

        auto full = offline::synthesizeAncestors(stored);
        auto secs = offline::buildSections(full);
        CHECK(secs.size() == 1);
        CHECK(secs.size() == 1 && secs[0].key == "5" && secs[0].title == "Series");
        auto top = offline::sectionItems(full, "5");
        CHECK(top.size() == 1 && top[0].ratingKey == "744");
        auto seasons = offline::childrenOf(full, "744");
        CHECK(seasons.size() == 1 && seasons[0].ratingKey == "745");
        auto eps = offline::childrenOf(full, "745");
        CHECK(eps.size() == 2 && eps[0].ratingKey == "747" && eps[1].ratingKey == "746");

        // second episode of a captured season: only its own fiche
        stored.erase(std::remove_if(stored.begin(), stored.end(),
                         [](const plex::Item& it) { return it.ratingKey == "746"; }),
            stored.end());
        size_t before = stored.size();
        offline::capture("746", series, fetch, children, has, put);
        CHECK(stored.size() == before + 1 && stored.back().ratingKey == "746");

        // a movie keeps a library its metadata already carries (Plex)
        stored.clear();
        server["743"].librarySectionID = "1";
        server["743"].librarySectionTitle = "Films";
        offline::capture("743", series, fetch, children, has, put);
        CHECK(stored.size() == 1 && stored[0].librarySectionID == "1" && stored[0].librarySectionTitle == "Films");

        // an unreachable show never loses the leaf already put
        stored.clear();
        failing.insert("744");
        bool threw = false;
        try {
            offline::capture("747", series, fetch, children, has, put);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(stored.size() == 1 && stored[0].ratingKey == "747");

        // unknown leaf: nothing
        stored.clear();
        offline::capture("999", series, fetch, children, has, put);
        CHECK(stored.empty());
    }

    // playQueue: downloaded episodes of the same show, in (season, episode)
    // order, across seasons; other shows and non-downloaded episodes left out
    {
        std::vector<plex::Item> eps = {legacyEpisode("s2e1", "Lost", 2, 1), legacyEpisode("s1e3", "Lost", 1, 3),
            legacyEpisode("s1e1", "Lost", 1, 1), legacyEpisode("s1e2", "Lost", 1, 2),
            legacyEpisode("o1", "Other", 1, 1), movie("mv", "Heat", "1")};
        auto tree = offline::synthesizeAncestors(eps);
        auto downloaded = [](const std::string& k) { return k != "s1e2"; };

        auto q = offline::playQueue(tree, "s1e1", downloaded);
        CHECK(q.size() == 3);
        if (q.size() == 3) {
            CHECK(q[0].ratingKey == "s1e1");
            CHECK(q[1].ratingKey == "s1e3");
            CHECK(q[2].ratingKey == "s2e1");
        }
        // started from a later episode: same queue, the caller finds its index
        CHECK(offline::playQueue(tree, "s2e1", downloaded).size() == 3);
        // a show alone in its queue
        CHECK(offline::playQueue(tree, "o1", downloaded).size() == 1);
        // a movie or an unknown key has no queue
        CHECK(offline::playQueue(tree, "mv", downloaded).empty());
        CHECK(offline::playQueue(tree, "nope", downloaded).empty());
    }

    if (failures == 0) {
        printf("test_offline_catalog: OK\n");
        return 0;
    }
    printf("test_offline_catalog: %d FAILURE(S)\n", failures);
    return 1;
}
