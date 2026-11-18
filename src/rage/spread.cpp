#include "skeet/rage/spread.hpp"
#include <bit>
#include <cmath>
#include <xmmintrin.h>

namespace skeet::rage {
namespace {
constexpr float full_turn = 6.283185482025146484375f;
constexpr std::array<std::uint8_t, 128> cache_seeds{
    2,   3,   4,   10,  13,  15,  27,  29, 37, 38,  41,  46,  47,  49,  50,  52,  56,  57,  58,
    62,  66,  68,  70,  72,  76,  83,  87, 97, 108, 109, 110, 111, 113, 121, 122, 125, 135, 1,
    5,   6,   8,   17,  25,  26,  28,  31, 32, 36,  39,  44,  51,  61,  67,  71,  74,  77,  78,
    79,  80,  81,  84,  89,  90,  91,  92, 96, 98,  101, 102, 104, 107, 114, 115, 119, 9,   11,
    16,  19,  20,  22,  30,  48,  54,  59, 60, 65,  69,  73,  75,  82,  85,  86,  88,  94,  99,
    103, 105, 112, 116, 117, 118, 124, 0,  7,  12,  14,  18,  21,  23,  24,  33,  34,  35,  40,
    42,  43,  45,  53,  55,  63,  64,  93, 95, 100, 106, 120, 133, 137};
float limit_inaccuracy(float value) noexcept {
    return _mm_cvtss_f32(_mm_min_ss(_mm_set_ss(1.0f), _mm_set_ss(value)));
}
RageSpreadSample combine(const SpreadSeed& sample, const SpreadParameters& parameters,
                         float inaccuracy) noexcept {
    const float first = transform_spread_radius(parameters.weapon_id, parameters.fire_mode,
                                                parameters.recoil_index, sample.first_radius) *
                        inaccuracy;
    const float second = transform_spread_radius(parameters.weapon_id, parameters.fire_mode,
                                                 parameters.recoil_index, sample.second_radius) *
                         parameters.spread;
    return {second * sample.second_cosine + first * sample.first_cosine,
            second * sample.second_sine + first * sample.first_sine};
}
}
void fill_spread_plane(std::span<SpreadSeed, 256> table, bool shotgun_patterns,
                       const SpreadRandom& random, const RageTrigonometry& trig) {
    for (unsigned seed = 0; seed < 256; ++seed) {
        auto& sample = table[seed];
        random.seed(random.context, std::int32_t(seed + 1));
        sample.first_radius = random.uniform(random.context, 0.0f, 1.0f);
        const float first_angle = random.uniform(random.context, 0.0f, full_turn);
        sample.first_sine = trig.sine(first_angle);
        sample.first_cosine = trig.cosine(first_angle);
        float second_angle;
        if (!shotgun_patterns) {
            sample.second_radius = random.uniform(random.context, 0.0f, 1.0f);
            second_angle = random.uniform(random.context, 0.0f, full_turn);
        } else {
            second_angle = random.uniform(random.context, 0.0f, full_turn);
            sample.second_radius = random.uniform(random.context, 0.0f, 1.0f);
        }
        sample.second_sine = trig.sine(second_angle);
        sample.second_cosine = trig.cosine(second_angle);
    }
}
float transform_spread_radius(std::uint32_t weapon, std::int32_t mode, float recoil,
                              float radius) noexcept {
    if (weapon == 0x40) {
        if (mode == 1)
            return 1.0f - radius * radius;
    } else if (weapon == 0x1c && recoil < 3.0f) {
        int iteration = 3;
        do {
            --iteration;
            radius *= radius;
        } while (float(iteration) > recoil);
        return 1.0f - radius;
    }
    return radius;
}
RageSpreadSample generate_spread_sample(const SpreadParameters& parameters, std::uint32_t seed,
                                        const SpreadTables& tables, const SpreadRandom& random,
                                        const ShotgunPattern& pattern,
                                        const RageTrigonometry& trig) {
    const float inaccuracy = limit_inaccuracy(parameters.inaccuracy);
    if (!parameters.shotgun_patterns)
        return combine(tables.standard[seed], parameters, inaccuracy);
    if (!pattern.get || parameters.pellet_count <= 1)
        return combine(tables.shotgun_patterns[seed], parameters, inaccuracy);
    random.seed(random.context, std::bit_cast<std::int32_t>(seed + 1));
    const auto recoil = _mm_cvttss_si32(_mm_set_ss(parameters.recoil_index));
    const auto first_pellet = std::uint32_t(recoil) * std::uint32_t(parameters.pellet_count);
    RageSpreadSample sum{0.0f, 0.0f};
    for (std::int32_t i = 0; i < parameters.pellet_count; ++i) {
        SpreadSeed sample;
        sample.first_radius = random.uniform(random.context, 0.0f, 1.0f);
        const float angle = random.uniform(random.context, 0.0f, full_turn);
        sample.first_sine = trig.sine(angle);
        sample.first_cosine = trig.cosine(angle);
        float second_angle = 0.0f;
        sample.second_radius = 0.0f;
        pattern.get(pattern.context, parameters.weapon_id, parameters.fire_mode,
                    std::bit_cast<std::int32_t>(first_pellet + std::uint32_t(i)), second_angle,
                    sample.second_radius);
        sample.second_sine = trig.sine(second_angle);
        sample.second_cosine = trig.cosine(second_angle);
        const auto point = combine(sample, parameters, inaccuracy);
        sum.first = point.first + sum.first;
        sum.second = point.second + sum.second;
    }
    const float inverse = 1.0f / float(parameters.pellet_count);
    return {sum.first * inverse, sum.second * inverse};
}
bool prepare_spread_cache(SpreadCache& cache, const SpreadParameters& parameters,
                          float combined_spread, std::uint32_t generation, bool enabled,
                          const SpreadTables& tables, const SpreadRandom& random,
                          const ShotgunPattern& pattern, const RageTrigonometry& trig) {
    if (!enabled || cache.generation == generation)
        return false;
    auto compensated = parameters;
    compensated.inaccuracy = combined_spread - parameters.spread;
    // 433C1B49 forces one pellet outside the class-4 shotgun cache branch.
    if (parameters.weapon_type != 4 || parameters.pellet_count <= 1)
        compensated.pellet_count = 1;
    for (unsigned i = 0; i < 128; ++i) {
        cache.primary[i] =
            generate_spread_sample(parameters, cache_seeds[i], tables, random, pattern, trig);
        cache.compensated[i] =
            generate_spread_sample(compensated, cache_seeds[i], tables, random, pattern, trig);
    }
    cache.generation = generation;
    return true;
}
std::array<RageSpreadSample, 32> hitchance_spread_samples(const SpreadCache& cache) noexcept {
    std::array<RageSpreadSample, 32> result;
    for (unsigned i = 0; i < 32; ++i)
        result[i] = cache.compensated[i * 4];
    return result;
}
std::uint32_t cached_spread_seed(std::uint32_t index) noexcept {
    return cache_seeds[index];
}
bool prepare_complete_spread(CompleteSpread& result, const SpreadParameters& parameters,
                              const SpreadTables& tables, const SpreadRandom& random,
                              const ShotgunPattern& pattern, const RageTrigonometry& trig) {
    result.valid = false;
    result.pellets = 0;
    if (parameters.pellet_count < 1 || parameters.pellet_count > int(CompleteSpread::maximum_pellets) ||
        !std::isfinite(parameters.inaccuracy) || parameters.inaccuracy < 0 ||
        !std::isfinite(parameters.spread) || parameters.spread < 0 ||
        !std::isfinite(parameters.recoil_index) || parameters.recoil_index < 0 ||
        parameters.recoil_index > 100000)
        return false;
    const unsigned pellets = unsigned(parameters.pellet_count);
    if (pellets == 1) {
        if (!tables.standard.data() || !tables.shotgun_patterns.data())
            return false;
        for (unsigned seed = 0; seed < CompleteSpread::seed_count; ++seed)
            result.rays[seed][0] = generate_spread_sample(parameters, seed, tables, random, pattern, trig);
    } else {
        if (!random.seed || !random.uniform || (parameters.shotgun_patterns && !pattern.get))
            return false;
        // Installed server.dll RVA 4C4BA6..4C4F64 seeds once per shot. Without
        // patterns, pellets share the first inaccuracy draw; their spread draws
        // advance the stream. Pattern mode draws inaccuracy for every pellet.
        const auto first_pellet = int(parameters.recoil_index) * int(pellets);
        for (unsigned seed = 0; seed < CompleteSpread::seed_count; ++seed) {
            random.seed(random.context, int(seed + 1));
            SpreadSeed sample{};
            for (unsigned pellet = 0; pellet < pellets; ++pellet) {
                if (!pellet || parameters.shotgun_patterns) {
                    sample.first_radius = random.uniform(random.context, 0, 1);
                    const auto angle = random.uniform(random.context, 0, full_turn);
                    sample.first_sine = trig.sine(angle);
                    sample.first_cosine = trig.cosine(angle);
                }
                float angle = 0;
                if (parameters.shotgun_patterns) {
                    sample.second_radius = 0;
                    pattern.get(pattern.context, parameters.weapon_id, parameters.fire_mode,
                                first_pellet + int(pellet), angle, sample.second_radius);
                } else {
                    sample.second_radius = random.uniform(random.context, 0, 1);
                    angle = random.uniform(random.context, 0, full_turn);
                }
                sample.second_sine = trig.sine(angle);
                sample.second_cosine = trig.cosine(angle);
                result.rays[seed][pellet] = combine(sample, parameters, limit_inaccuracy(parameters.inaccuracy));
            }
        }
    }
    for (const auto& seed : result.rays)
        for (unsigned pellet = 0; pellet < pellets; ++pellet)
            if (!std::isfinite(seed[pellet].first) || !std::isfinite(seed[pellet].second))
                return false;
    result.pellets = pellets;
    return result.valid = true;
}
} // namespace skeet::rage
