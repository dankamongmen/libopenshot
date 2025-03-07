/**
 * @file
 * @brief Source file for PlayerPrivate class
 * @author Duzy Chan <code@duzy.info>
 * @author Jonathan Thomas <jonathan@openshot.org>
 *
 * @ref License
 */

// Copyright (c) 2008-2019 OpenShot Studios, LLC
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "PlayerPrivate.h"
#include "Exceptions.h"
#include "ZmqLogger.h"

#include <thread>    // for std::this_thread::sleep_for
#include <chrono>    // for std::chrono milliseconds, high_resolution_clock

namespace openshot
{
    // Constructor
    PlayerPrivate::PlayerPrivate(openshot::RendererBase *rb)
    : renderer(rb), Thread("player"), video_position(1), audio_position(0)
    , audioPlayback(new openshot::AudioPlaybackThread())
    , videoPlayback(new openshot::VideoPlaybackThread(rb))
    , videoCache(new openshot::VideoCacheThread())
    , speed(1), reader(NULL), last_video_position(1), max_sleep_ms(3000)
    { }

    // Destructor
    PlayerPrivate::~PlayerPrivate()
    {
        stopPlayback();
        delete audioPlayback;
        delete videoCache;
        delete videoPlayback;
    }

    // Start thread
    void PlayerPrivate::run()
    {
        // bail if no reader set
        if (!reader)
            return;

        // Start the threads
        if (reader->info.has_audio)
            audioPlayback->startThread(8);
        if (reader->info.has_video) {
            videoCache->startThread(2);
            videoPlayback->startThread(4);
        }

        using std::chrono::duration_cast;

        // Types for storing time durations in whole and fractional milliseconds
        using ms = std::chrono::milliseconds;
        using double_ms = std::chrono::duration<double, ms::period>;

        // Calculate on-screen time for a single frame in milliseconds
        const auto frame_duration = double_ms(1000.0 / reader->info.fps.ToDouble());

        while (!threadShouldExit()) {
            // Get the start time (to track how long a frame takes to render)
            const auto time1 = std::chrono::high_resolution_clock::now();

            // Get the current video frame (if it's different)
            frame = getFrame();

            // Experimental Pausing Code (if frame has not changed)
            if ((speed == 0 && video_position == last_video_position)
                || (video_position > reader->info.video_length)
               ) {
                speed = 0;
                std::this_thread::sleep_for(frame_duration);
                continue;
            }

            // Set the video frame on the video thread and render frame
            videoPlayback->frame = frame;
            videoPlayback->render.signal();

            // Keep track of the last displayed frame
            last_video_position = video_position;

            // How many frames ahead or behind is the video thread?
            int64_t video_frame_diff = 0;
            if (reader->info.has_audio && reader->info.has_video) {
                if (speed != 1)
                    // Set audio frame again (since we are not in normal speed, and not paused)
                    audioPlayback->Seek(video_position);

                // Only calculate this if a reader contains both an audio and video thread
                audio_position = audioPlayback->getCurrentFramePosition();
                video_frame_diff = video_position - audio_position;
            }

            // Get the end time (to track how long a frame takes to render)
            const auto time2 = std::chrono::high_resolution_clock::now();

            // Determine how many milliseconds it took to render the frame
            const auto render_time = double_ms(time2 - time1);

            // Calculate the amount of time to sleep (by subtracting the render time)
            auto sleep_time = duration_cast<ms>(frame_duration - render_time);

            // Debug
            ZmqLogger::Instance()->AppendDebugMethod("PlayerPrivate::run (determine sleep)", "video_frame_diff", video_frame_diff, "video_position", video_position, "audio_position", audio_position, "speed", speed, "render_time(ms)", render_time.count(), "sleep_time(ms)", sleep_time.count());

            // Adjust drift (if more than a few frames off between audio and video)
            if (video_frame_diff > 0 && reader->info.has_audio && reader->info.has_video) {
                // Since the audio and video threads are running independently,
                // they will quickly get out of sync. To fix this, we calculate
                // how far ahead or behind the video frame is, and adjust the amount
                // of time the frame is displayed on the screen (i.e. the sleep time).
                // If a frame is ahead of the audio, we sleep for longer.
                // If a frame is behind the audio, we sleep less (or not at all),
                // in order for the video to catch up.
                sleep_time += duration_cast<ms>(video_frame_diff * frame_duration);
            }

            else if (video_frame_diff < -10 && reader->info.has_audio && reader->info.has_video) {
                // Skip frame(s) to catch up to the audio (if more than 10 frames behind)
                video_position += std::fabs(video_frame_diff) / 2; // Seek forward 1/2 the difference
                sleep_time = sleep_time.zero(); // Don't sleep now... immediately go to next position
            }

            // Sleep (leaving the video frame on the screen for the correct amount of time)
            // Don't sleep too long though (in some extreme cases, for example when stopping threads
            // and shutting down, the video_frame_diff can jump to a crazy big number, and we don't
            // want to sleep too long (max of X seconds)
            if (sleep_time > sleep_time.zero() && sleep_time.count() < max_sleep_ms) {
                std::this_thread::sleep_for(sleep_time);
            }

        }
    }

    // Get the next displayed frame (based on speed and direction)
    std::shared_ptr<openshot::Frame> PlayerPrivate::getFrame()
    {
    try {
        // Get the next frame (based on speed)
        if (video_position + speed >= 1 && video_position + speed <= reader->info.video_length)
            video_position = video_position + speed;

        if (frame && frame->number == video_position && video_position == last_video_position) {
            // return cached frame
            return frame;
        }
        else
        {
            // Update cache on which frame was retrieved
            videoCache->setCurrentFramePosition(video_position);

            // return frame from reader
            return reader->GetFrame(video_position);
        }

    } catch (const ReaderClosed & e) {
        // ...
    } catch (const OutOfBoundsFrame & e) {
        // ...
    }
    return std::shared_ptr<openshot::Frame>();
    }

    // Start video/audio playback
    bool PlayerPrivate::startPlayback()
    {
        if (video_position < 0) return false;

        stopPlayback();
        startThread(1);
        return true;
    }

    // Stop video/audio playback
    void PlayerPrivate::stopPlayback()
    {
        if (audioPlayback->isThreadRunning() && reader->info.has_audio) audioPlayback->stopThread(max_sleep_ms);
        if (videoCache->isThreadRunning() && reader->info.has_video) videoCache->stopThread(max_sleep_ms);
        if (videoPlayback->isThreadRunning() && reader->info.has_video) videoPlayback->stopThread(max_sleep_ms);
        if (isThreadRunning()) stopThread(max_sleep_ms);
    }

}
