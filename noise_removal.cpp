extern "C" {
#include <libavcodec/avcodec.h>  // Includes the codecs for audio and video processing
#include <libavformat/avformat.h> // Includes functionality for format handling
#include <libavutil/samplefmt.h>  // Utility for handling audio sample formats
#include <libavutil/common.h>     // Common utility functions
#include <libavutil/channel_layout.h> // Channel layout utility
#include <libavutil/imgutils.h>       // Image utilities (not directly needed here)
#include <libavutil/opt.h>            // Option handling functionality
#include <libavutil/mathematics.h>    // Math utilities for audio/video processing
#include <libavutil/frame.h>          // Frame handling for audio/video
#include <libswresample/swresample.h> // Handling audio resampling
}

#include <speex/speex_preprocess.h> // SpeexDSP library for audio processing
#include <iostream> // Standard C++ library for input/output

// Entry point of the program
int main(int argc, char* argv[]) {
    // Check if the user has entered the correct number of arguments
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <input file> <output file>\n";
        return -1;
    }

    // File names from the command line arguments
    const char* input_filename = argv[1];
    const char* output_filename = argv[2];

    // Initialize format context for input file
    AVFormatContext* format_context = nullptr;
    if (avformat_open_input(&format_context, input_filename, nullptr, nullptr) != 0) {
        std::cerr << "Could not open input file " << input_filename << "\n";
        return -1;
    }

    // Retrieve stream information from input file
    if (avformat_find_stream_info(format_context, nullptr) < 0) {
        std::cerr << "Failed to retrieve input stream information\n";
        return -1;
    }

    // Identify the audio stream index
    int audio_stream_index = -1;
    for (unsigned int i = 0; i < format_context->nb_streams; i++) {
        if (format_context->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }

    // Handle error if no audio stream is found
    if (audio_stream_index == -1) {
        std::cerr << "Could not find audio stream\n";
        return -1;
    }

    // Codec parameters for the audio stream
    AVCodecParameters* codec_params = format_context->streams[audio_stream_index]->codecpar;
    AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "Codec not found\n";
        return -1;
    }

    // Context setup for the codec
    AVCodecContext* codec_context = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_context, codec_params);
    if (avcodec_open2(codec_context, codec, nullptr) < 0) {
        std::cerr << "Could not open codec\n";
        return -1;
    }

    // Allocation for packets and frames
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    // Setup Speex preprocessor state for noise suppression
    SpeexPreprocessState* preprocess_state = speex_preprocess_state_init(codec_context->frame_size, codec_context->sample_rate);
    int denoise = 1;
    int noiseSuppress = -10; // Suppress noise by -30 dB
    speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    speex_preprocess_ctl(preprocess_state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &noiseSuppress);

    // Prepare the output format context for the MP3 encoder
    AVFormatContext* output_format_context = nullptr;
    AVStream* out_stream = nullptr;
    avformat_alloc_output_context2(&output_format_context, nullptr, "mp3", output_filename);
    if (!output_format_context) {
        std::cerr << "Could not create output context\n";
        return -1;
    }

    // Find the MP3 encoder
    AVCodec* output_codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    if (!output_codec) {
        std::cerr << "MP3 encoder not found\n";
        return -1;
    }

    // Create a new stream in the output file
    out_stream = avformat_new_stream(output_format_context, output_codec);
    if (!out_stream) {
        std::cerr << "Failed to create a new stream for output\n";
        return -1;
    }

    // Setup the codec context for the output encoder
    AVCodecContext* out_codec_context = avcodec_alloc_context3(output_codec);
    out_codec_context->sample_rate = codec_context->sample_rate;
    out_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    out_codec_context->channels = av_get_channel_layout_nb_channels(out_codec_context->channel_layout);
    out_codec_context->sample_fmt = output_codec->sample_fmts[0];
    out_codec_context->bit_rate = 192000;

    // Open the output codec
    if (avcodec_open2(out_codec_context, output_codec, nullptr) < 0) {
        std::cerr << "Could not open the encoder\n";
        return -1;
    }

    // Ensure proper codec tag and header flags
    out_stream->codecpar->codec_tag = 0;
    if (output_format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        out_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Transfer codec parameters to the stream
    avcodec_parameters_from_context(out_stream->codecpar, out_codec_context);

    // Open the output file
    if (!(output_format_context->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&output_format_context->pb, output_filename, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "Could not open output file " << output_filename << "\n";
            return -1;
        }
    }

    // Write header to the output file
    if (avformat_write_header(output_format_context, nullptr) < 0) {
        std::cerr << "Error occurred when opening output file\n";
        return -1;
    }

    // Allocate output packet
    AVPacket* out_packet = av_packet_alloc();

    // Main loop to read from input file, process, and encode to output file
    while (av_read_frame(format_context, packet) >= 0) {
        if (packet->stream_index == audio_stream_index) {
            if (avcodec_send_packet(codec_context, packet) == 0) {
                while (avcodec_receive_frame(codec_context, frame) == 0) {
                    // Process frame with Speex for noise suppression
                    speex_preprocess_run(preprocess_state, (spx_int16_t*)frame->data[0]);

                    // Encode processed frame to MP3
                    if (avcodec_send_frame(out_codec_context, frame) == 0) {
                        while (avcodec_receive_packet(out_codec_context, out_packet) == 0) {
                            av_interleaved_write_frame(output_format_context, out_packet);
                            av_packet_unref(out_packet);
                        }
                    }
                }
            }
        }
        av_packet_unref(packet);
    }

    // Write trailer to output file
    av_write_trailer(output_format_context);

    // Cleanup and free resources
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codec_context);
    avformat_close_input(&format_context);
    speex_preprocess_state_destroy(preprocess_state);
    avcodec_close(out_codec_context);
    avformat_free_context(output_format_context);

    std::cout << "Processing completed.\n";
    return 0;
}
