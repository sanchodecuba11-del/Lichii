// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <charconv>
#include <cstdlib>
#include <mutex>
#include <string>

#include "common/logging.h"
#include "common/nx2_switches.h"

namespace Common::Nx2Switches {

namespace {

constexpr std::array<Info, static_cast<size_t>(Id::Count)> infos{{
    {"list_words", "#1336 : la carte lit les mots de listes qu'elle a ecrits (dessins et calculs)", 0, false, true},
    {"fast_draw_neutral", "#1216 : plus de dessins gardent le chemin rapide (dessins reutilises)", 1, false, true},
    {"astc_even_bc", "#1194 : ASTC a mips irreguliers recompresses en BC sans la queue (AMD) - au prochain lancement", 0, false, false},
    {"amd_draw_budget", "#1088 : 512 dessins par envoi sur le pilote AMD", 0, false, true},
    {"amd_query_availability", "#1087 : resultats de requetes copies avec leur disponibilite (AMD)", 0, false, true},
    {"image_pool", "#922 : images de 8 a 16 Mio dans un pool borne de blocs de 64 Mio", 0, false, true},
    {"list_waits", "#1094 (sans effet ici : #1161 a sa version) : les listes n'attendent que les mots ecrits par la carte, plus les ecrivains d'avant la derniere synchro", 1, false, true},
    {"acq_park", "#1477 (sans effet ici : #1161 a sa version) : une acquisition non satisfaite met le canal en pause sans vider la traduction", 1, false, true},
    {"fast_draw_attribs", "Traduction : un format d'attribut de sommet qui garde son type laisse le dessin sur le chemin rapide", 1, false, true},
    {"fast_draw_viewports", "Traduction : fenetres, decoupes, couleur de melange, biais de profondeur, gabarit, largeur de ligne laissent le dessin sur le chemin rapide", 1, false, true},
    {"fast_draw_writes", "Traduction : jusqu'a 64 ecritures de constantes entre deux dessins sur le chemin rapide (16 avant)", 1, false, true},
    {"fast_draw_serial", "Traduction : une seule operation atomique par intervalle entre deux dessins pour les cassures du chemin rapide", 1, false, true},
    {"fast_draw_unread", "Traduction : un attribut de sommet que le shader ne lit pas ne casse plus le chemin rapide", 1, false, true},
    {"wfi_dedup", "Traduction : une invalidation sans rien d'enregistre depuis la precedente ne pose pas de nouvelle barriere", 1, false, true},
    {"wfi_keep_pass", "Traduction : une invalidation en pleine passe de rendu, sans ecriture de stockage depuis la derniere barriere, garde la passe ouverte", 1, false, true},
    {"report_light", "Traduction : les rapports ne font plus d'allocation sur le tas", 1, false, true},
    {"direct_sink", "Traduction : les ecritures de registres par les macros passent par la mise de cote", 1, false, true},
    {"gpu_profile", "Mesure : temps des fils GPU par zone, toutes les 10 s", 0, true, true},
    {"stutter_trace", "Mesure : chaque image de plus de 33 ms et sa cause ; resume toutes les 10 s", 0, true, true},
    {"frame_trace", "Mesure : par image, temps processeur de chaque fil, temps de la carte", 0, true, true},
    {"host_timing", "Mesure : retard des minuteries, attentes du fil des minuteries, chaque seconde", 0, true, true},
    {"guest_wait_trace", "Mesure : chaque attente du jeu sur la carte, sa duree, le temps de l'envoi au signal ; toutes les 2 s", 0, true, true},
    {"park_trace", "Mesure : ce qui reveille un canal en pause, et apres combien de temps ; toutes les 2 s", 0, true, true},
    {"thread_trace", "Mesure : ou attendent les fils du jeu, ses sommeils, ses reveils", 0, true, true},
    {"other_trace", "Mesure : le poste autre de la traduction, par genre d'entree de l'anneau (avec gpu_profile) ; toutes les 10 s", 0, true, true},
}};

std::mutex init_mutex;

} // Anonymous namespace

const Info& Describe(Id id) {
    return infos[static_cast<size_t>(id)];
}

std::optional<Id> Find(std::string_view name) {
    for (size_t i = 0; i < infos.size(); ++i) {
        if (name == infos[i].name) {
            return static_cast<Id>(i);
        }
    }
    return std::nullopt;
}

void Set(Id id, int value) {
    if (!detail::g_initialized.load(std::memory_order_acquire)) {
        detail::Initialize();
    }
    detail::g_values[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
    LOG_INFO(Common, "Switch {} = {}", infos[static_cast<size_t>(id)].name, value);
}

bool Set(std::string_view name, int value) {
    const auto id = Find(name);
    if (!id) {
        return false;
    }
    Set(*id, value);
    return true;
}

namespace detail {

void Initialize() {
    std::scoped_lock lock{init_mutex};
    if (g_initialized.load(std::memory_order_relaxed)) {
        return;
    }
    for (size_t i = 0; i < infos.size(); ++i) {
        g_values[i].store(infos[i].default_value, std::memory_order_relaxed);
    }
    // EDEN_NX2_SWITCHES="name=value,name=value"
    if (const char* text = std::getenv("EDEN_NX2_SWITCHES")) {
        std::string_view rest{text};
        while (!rest.empty()) {
            const size_t comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            const size_t equals = item.find('=');
            const std::string_view name = item.substr(0, equals);
            int value = 1;
            if (equals != std::string_view::npos) {
                const std::string_view digits = item.substr(equals + 1);
                std::from_chars(digits.data(), digits.data() + digits.size(), value);
            }
            if (const auto id = Find(name)) {
                g_values[static_cast<size_t>(*id)].store(value, std::memory_order_relaxed);
            }
        }
    }
    g_initialized.store(true, std::memory_order_release);
}

} // namespace detail

} // namespace Common::Nx2Switches
