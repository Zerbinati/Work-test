/*
  HypnoS, a UCI chess playing engine derived from Stockfish
  Copyright (C) 2004-2025 The Stockfish developers (see AUTHORS file)

  HypnoS is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  HypnoS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Code for calculating NNUE evaluation function

#include "evaluate_nnue.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "../evaluate.h"
#include "../misc.h"
#include "../position.h"
#include "../types.h"
#include "../uci.h"
#include "nnue_accumulator.h"
#include "nnue_common.h"
#include "evaluate_nnue.h"

namespace Hypnos::Eval::NNUE {

// Input feature converter
LargePagePtr<FeatureTransformer<TransformedFeatureDimensionsBig, &StateInfo::accumulatorBig>>
  featureTransformerBig;
LargePagePtr<FeatureTransformer<TransformedFeatureDimensionsSmall, &StateInfo::accumulatorSmall>>
  featureTransformerSmall;

// Evaluation function
AlignedPtr<Network<TransformedFeatureDimensionsBig, L2Big, L3Big>>       networkBig[LayerStacks];
AlignedPtr<Network<TransformedFeatureDimensionsSmall, L2Small, L3Small>> networkSmall[LayerStacks];

// Evaluation function file names
std::string fileName[2];
std::string netDescription[2];

namespace Detail {

// Initialize the evaluation function parameters
template<typename T>
void initialize(AlignedPtr<T>& pointer) {

    pointer.reset(reinterpret_cast<T*>(std_aligned_alloc(alignof(T), sizeof(T))));
    std::memset(pointer.get(), 0, sizeof(T));
}

template<typename T>
void initialize(LargePagePtr<T>& pointer) {

    static_assert(alignof(T) <= 4096,
                  "aligned_large_pages_alloc() may fail for such a big alignment requirement of T");
    pointer.reset(reinterpret_cast<T*>(aligned_large_pages_alloc(sizeof(T))));
    std::memset(pointer.get(), 0, sizeof(T));
}

// Read evaluation function parameters
template<typename T>
bool read_parameters(std::istream& stream, T& reference) {

    std::uint32_t header;
    header = read_little_endian<std::uint32_t>(stream);
    if (!stream || header != T::get_hash_value())
        return false;
    return reference.read_parameters(stream);
}

// Write evaluation function parameters
template<typename T>
bool write_parameters(std::ostream& stream, const T& reference) {

    write_little_endian<std::uint32_t>(stream, T::get_hash_value());
    return reference.write_parameters(stream);
}

}  // namespace Detail


// Initialize the evaluation function parameters
static void initialize(NetSize netSize) {

    if (netSize == Small)
    {
        Detail::initialize(featureTransformerSmall);
        for (std::size_t i = 0; i < LayerStacks; ++i)
            Detail::initialize(networkSmall[i]);
    }
    else
    {
        Detail::initialize(featureTransformerBig);
        for (std::size_t i = 0; i < LayerStacks; ++i)
            Detail::initialize(networkBig[i]);
    }
}

// Read network header
static bool read_header(std::istream& stream, std::uint32_t* hashValue, std::string* desc) {
    std::uint32_t version, size;

    version    = read_little_endian<std::uint32_t>(stream);
    *hashValue = read_little_endian<std::uint32_t>(stream);
    size       = read_little_endian<std::uint32_t>(stream);
    if (!stream || version != Version)
        return false;
    desc->resize(size);
    stream.read(&(*desc)[0], size);
    return !stream.fail();
}

// Write network header
static bool write_header(std::ostream& stream, std::uint32_t hashValue, const std::string& desc) {
    write_little_endian<std::uint32_t>(stream, Version);
    write_little_endian<std::uint32_t>(stream, hashValue);
    write_little_endian<std::uint32_t>(stream, std::uint32_t(desc.size()));
    stream.write(&desc[0], desc.size());
    return !stream.fail();
}

// Read network parameters
static bool read_parameters(std::istream& stream, NetSize netSize) {

    std::uint32_t hashValue;
    if (!read_header(stream, &hashValue, &netDescription[netSize]))
        return false;
    if (hashValue != HashValue[netSize])
        return false;
    if (netSize == Big && !Detail::read_parameters(stream, *featureTransformerBig))
        return false;
    if (netSize == Small && !Detail::read_parameters(stream, *featureTransformerSmall))
        return false;
    for (std::size_t i = 0; i < LayerStacks; ++i)
    {
        if (netSize == Big && !Detail::read_parameters(stream, *(networkBig[i])))
            return false;
        if (netSize == Small && !Detail::read_parameters(stream, *(networkSmall[i])))
            return false;
    }
    return stream && stream.peek() == std::ios::traits_type::eof();
}

// Write network parameters
static bool write_parameters(std::ostream& stream, NetSize netSize) {

    if (!write_header(stream, HashValue[netSize], netDescription[netSize]))
        return false;
    if (netSize == Big && !Detail::write_parameters(stream, *featureTransformerBig))
        return false;
    if (netSize == Small && !Detail::write_parameters(stream, *featureTransformerSmall))
        return false;
    for (std::size_t i = 0; i < LayerStacks; ++i)
    {
        if (netSize == Big && !Detail::write_parameters(stream, *(networkBig[i])))
            return false;
        if (netSize == Small && !Detail::write_parameters(stream, *(networkSmall[i])))
            return false;
    }
    return bool(stream);
}

void hint_common_parent_position(const Position& pos) {

    int simpleEvalAbs = std::abs(simple_eval(pos, pos.side_to_move()));
    if (simpleEvalAbs > Eval::SmallNetThreshold)
        featureTransformerSmall->hint_common_access(pos, simpleEvalAbs > Eval::PsqtOnlyThreshold);
    else
        featureTransformerBig->hint_common_access(pos, false);
}

// Evaluation function. Perform differential calculation.
template<NetSize Net_Size>
Value evaluate(const Position& pos, bool adjusted, int* complexity, bool psqtOnly) {

    // We manually align the arrays on the stack because with gcc < 9.3
    // overaligning stack variables with alignas() doesn't work correctly.

    constexpr uint64_t alignment = CacheLineSize;
    constexpr int      delta     = 24;

#if defined(ALIGNAS_ON_STACK_VARIABLES_BROKEN)
    TransformedFeatureType transformedFeaturesUnaligned
      [FeatureTransformer < Net_Size == Small ? TransformedFeatureDimensionsSmall
                                              : TransformedFeatureDimensionsBig,
       nullptr > ::BufferSize + alignment / sizeof(TransformedFeatureType)];

    auto* transformedFeatures = align_ptr_up<alignment>(&transformedFeaturesUnaligned[0]);
#else

    alignas(alignment) TransformedFeatureType
      transformedFeatures[FeatureTransformer < Net_Size == Small ? TransformedFeatureDimensionsSmall
                                                                 : TransformedFeatureDimensionsBig,
                          nullptr > ::BufferSize];
#endif

    ASSERT_ALIGNED(transformedFeatures, alignment);

    const int bucket = (pos.count<ALL_PIECES>() - 1) / 4;
    const auto psqt = Net_Size == Small
        ? featureTransformerSmall->transform(pos, transformedFeatures, bucket, psqtOnly)
        : featureTransformerBig->transform(pos, transformedFeatures, bucket, psqtOnly);

    const auto positional = !psqtOnly
        ? (Net_Size == Small ? networkSmall[bucket]->propagate(transformedFeatures)
                             : networkBig[bucket]->propagate(transformedFeatures))
        : 0;

    if (complexity)
        *complexity = !psqtOnly ? std::abs(psqt - positional) / OutputScale : 0;

    // Give more value to positional evaluation when adjusted flag is set
    if (adjusted)
        return static_cast<Value>(((1024 - delta + MaterialisticEvaluationStrategy) * psqt
                                   + (1024 + delta + PositionalEvaluationStrategy) * positional)
                                  / (1024 * OutputScale));
    else
        return static_cast<Value>((psqt + positional) / OutputScale);
}

template Value evaluate<Big>(const Position& pos, bool adjusted, int* complexity, bool psqtOnly);
template Value evaluate<Small>(const Position& pos, bool adjusted, int* complexity, bool psqtOnly);

struct NnueEvalTrace {
    static_assert(LayerStacks == PSQTBuckets);

    Value       psqt[LayerStacks];
    Value       positional[LayerStacks];
    std::size_t correctBucket;
};

static NnueEvalTrace trace_evaluate(const Position& pos) {

    // We manually align the arrays on the stack because with gcc < 9.3
    // overaligning stack variables with alignas() doesn't work correctly.
    constexpr uint64_t alignment = CacheLineSize;

#if defined(ALIGNAS_ON_STACK_VARIABLES_BROKEN)
    TransformedFeatureType transformedFeaturesUnaligned
      [FeatureTransformer<TransformedFeatureDimensionsBig, nullptr>::BufferSize
       + alignment / sizeof(TransformedFeatureType)];

    auto* transformedFeatures = align_ptr_up<alignment>(&transformedFeaturesUnaligned[0]);
#else
    alignas(alignment) TransformedFeatureType
      transformedFeatures[FeatureTransformer<TransformedFeatureDimensionsBig, nullptr>::BufferSize];
#endif

    ASSERT_ALIGNED(transformedFeatures, alignment);

    NnueEvalTrace t{};
    t.correctBucket = (pos.count<ALL_PIECES>() - 1) / 4;
    for (IndexType bucket = 0; bucket < LayerStacks; ++bucket)
    {
        const auto materialist =
          featureTransformerBig->transform(pos, transformedFeatures, bucket, false);
        const auto positional = networkBig[bucket]->propagate(transformedFeatures);

        t.psqt[bucket]       = static_cast<Value>(materialist / OutputScale);
        t.positional[bucket] = static_cast<Value>(positional / OutputScale);
    }

    return t;
}

constexpr std::string_view PieceToChar(" PNBRQK  pnbrqk");

// Converts a Value into (centi)pawns and writes it in a buffer.
// The buffer must have capacity for at least 5 chars.
static void format_cp_compact(Value v, char* buffer) {

    buffer[0] = (v < 0 ? '-' : v > 0 ? '+' : ' ');

    int cp = std::abs(UCI::to_cp(v));
    if (cp >= 10000)
    {
        buffer[1] = '0' + cp / 10000;
        cp %= 10000;
        buffer[2] = '0' + cp / 1000;
        cp %= 1000;
        buffer[3] = '0' + cp / 100;
        buffer[4] = ' ';
    }
    else if (cp >= 1000)
    {
        buffer[1] = '0' + cp / 1000;
        cp %= 1000;
        buffer[2] = '0' + cp / 100;
        cp %= 100;
        buffer[3] = '.';
        buffer[4] = '0' + cp / 10;
    }
    else
    {
        buffer[1] = '0' + cp / 100;
        cp %= 100;
        buffer[2] = '.';
        buffer[3] = '0' + cp / 10;
        cp %= 10;
        buffer[4] = '0' + cp / 1;
    }
}


// Converts a Value into pawns, always keeping two decimals
static void format_cp_aligned_dot(Value v, std::stringstream& stream) {

    const double pawns = std::abs(0.01 * UCI::to_cp(v));

    stream << (v < 0 ? '-' : v > 0 ? '+' : ' ') << std::setiosflags(std::ios::fixed)
           << std::setw(6) << std::setprecision(2) << pawns;
}


// Returns a string with the value of each piece on a board,
// and a table for (PSQT, Layers) values bucket by bucket.
std::string trace(Position& pos) {

    std::stringstream ss;

    char board[3 * 8 + 1][8 * 8 + 2];
    std::memset(board, ' ', sizeof(board));
    for (int row = 0; row < 3 * 8 + 1; ++row)
        board[row][8 * 8 + 1] = '\0';

    // A lambda to output one box of the board
    auto writeSquare = [&board](File file, Rank rank, Piece pc, Value value) {
        const int x = int(file) * 8;
        const int y = (7 - int(rank)) * 3;
        for (int i = 1; i < 8; ++i)
            board[y][x + i] = board[y + 3][x + i] = '-';
        for (int i = 1; i < 3; ++i)
            board[y + i][x] = board[y + i][x + 8] = '|';
        board[y][x] = board[y][x + 8] = board[y + 3][x + 8] = board[y + 3][x] = '+';
        if (pc != NO_PIECE)
            board[y + 1][x + 4] = PieceToChar[pc];
        if (value != VALUE_NONE)
            format_cp_compact(value, &board[y + 2][x + 2]);
    };

    // We estimate the value of each piece by doing a differential evaluation from
    // the current base eval, simulating the removal of the piece from its square.
    Value base = evaluate<NNUE::Big>(pos);
    base       = pos.side_to_move() == WHITE ? base : -base;

    for (File f = FILE_A; f <= FILE_H; ++f)
        for (Rank r = RANK_1; r <= RANK_8; ++r)
        {
            Square sq = make_square(f, r);
            Piece  pc = pos.piece_on(sq);
            Value  v  = VALUE_NONE;

            if (pc != NO_PIECE && type_of(pc) != KING)
            {
                auto st = pos.state();

                pos.remove_piece(sq);
                st->accumulatorBig.computed[WHITE]     = st->accumulatorBig.computed[BLACK] =
                st->accumulatorBig.computedPSQT[WHITE] = st->accumulatorBig.computedPSQT[BLACK] = false;

                Value eval = evaluate<NNUE::Big>(pos);
                eval       = pos.side_to_move() == WHITE ? eval : -eval;
                v          = base - eval;

                pos.put_piece(pc, sq);
                st->accumulatorBig.computed[WHITE]     = st->accumulatorBig.computed[BLACK] =
                st->accumulatorBig.computedPSQT[WHITE] = st->accumulatorBig.computedPSQT[BLACK] = false;
            }

            writeSquare(f, r, pc, v);
        }

    ss << " NNUE derived piece values:\n";
    for (int row = 0; row < 3 * 8 + 1; ++row)
        ss << board[row] << '\n';
    ss << '\n';

    auto t = trace_evaluate(pos);

    ss << " NNUE network contributions "
       << (pos.side_to_move() == WHITE ? "(White to move)" : "(Black to move)") << std::endl
       << "+------------+------------+------------+------------+\n"
       << "|   Bucket   |  Material  | Positional |   Total    |\n"
       << "|            |   (PSQT)   |  (Layers)  |            |\n"
       << "+------------+------------+------------+------------+\n";

    for (std::size_t bucket = 0; bucket < LayerStacks; ++bucket)
    {
        ss << "|  " << bucket << "        ";
        ss << " |  ";
        format_cp_aligned_dot(t.psqt[bucket], ss);
        ss << "  "
           << " |  ";
        format_cp_aligned_dot(t.positional[bucket], ss);
        ss << "  "
           << " |  ";
        format_cp_aligned_dot(t.psqt[bucket] + t.positional[bucket], ss);
        ss << "  "
           << " |";
        if (bucket == t.correctBucket)
            ss << " <-- this bucket is used";
        ss << '\n';
    }

    ss << "+------------+------------+------------+------------+\n";

    return ss.str();
}


// Load eval, from a file stream or a memory stream
bool load_eval(const std::string name, std::istream& stream, NetSize netSize) {

    initialize(netSize);
    fileName[netSize] = name;
    return read_parameters(stream, netSize);
}

// Save eval, to a file stream or a memory stream
bool save_eval(std::ostream& stream, NetSize netSize) {

    if (fileName[netSize].empty())
        return false;

    return write_parameters(stream, netSize);
}

// Save eval, to a file given by its name
bool save_eval(const std::optional<std::string>& filename, NetSize netSize) {

    std::string actualFilename;
    std::string msg;

    if (filename.has_value())
        actualFilename = filename.value();
    else
    {
        if (EvalFiles.at(netSize).selected_name
            != (netSize == Small ? EvalFileDefaultNameSmall : EvalFileDefaultNameBig))
        {
            msg = "Failed to export a net. "
                  "A non-embedded net can only be saved if the filename is specified";

            sync_cout << msg << sync_endl;
            return false;
        }
        actualFilename = (netSize == Small ? EvalFileDefaultNameSmall : EvalFileDefaultNameBig);
    }

    std::ofstream stream(actualFilename, std::ios_base::binary);
    bool          saved = save_eval(stream, netSize);

    msg = saved ? "Network saved successfully to " + actualFilename : "Failed to export a net";

    sync_cout << msg << sync_endl;
    return saved;
}


}  // namespace Hypnos::Eval::NNUE

