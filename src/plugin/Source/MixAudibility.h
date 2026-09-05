#pragma once

/*
 * Whether one lane of the monitoring mix is heard, given its own two flags
 * and whether anything in the mix is soloed.
 *
 * Its own header because two very different readers have to agree on it: the
 * mixer applies it per block on the audio thread, and the editor asks the
 * same question to decide whether to draw a lane's waveform dimmed. When they
 * disagreed the row said two things at once.
 */
namespace stemlab::mix
{

/**
 * Mute wins, soloed or not.
 *
 * The rule used to be "when anything is soloed, only solo counts", which made
 * the M button on a soloed lane light up and do nothing - the lane kept
 * playing and the editor kept drawing it at full brightness. Every DAW this
 * sits beside lets mute win, and a control that can light while having no
 * effect is worse than either rule on its own.
 */
constexpr bool isAudible(bool muted, bool soloed, bool anySolo)
{
    if (muted)
        return false;

    return !anySolo || soloed;
}

} // namespace stemlab::mix
