#include "reed_solomon.h"
#include "helpers.h"
#include "jerasure.h"

extern "C" {
#include <contrib/libs/jerasure/cauchy.h>
#include <contrib/libs/jerasure/jerasure.h>
}

#include <algorithm>

namespace NYT::NErasure {

////////////////////////////////////////////////////////////////////////////////

TCauchyReedSolomon::TCauchyReedSolomon(
    int dataPartCount,
    int parityPartCount,
    int wordSize)
    : DataPartCount_(dataPartCount)
    , ParityPartCount_(parityPartCount)
    , WordSize_(wordSize)
{
    // Check that the word size is sane.
    YCHECK(WordSize_ <= MaxWordSize);

    InitializeJerasure();

    Matrix_ = TMatrix(cauchy_good_general_coding_matrix(dataPartCount, parityPartCount, wordSize));
    BitMatrix_ = TMatrix(jerasure_matrix_to_bitmatrix(dataPartCount, parityPartCount, wordSize, Matrix_.Get()));
    Schedule_ = TSchedule(jerasure_smart_bitmatrix_to_schedule(dataPartCount, parityPartCount, wordSize, BitMatrix_.Get()));
}

std::vector<TSharedRef> TCauchyReedSolomon::Encode(const std::vector<TSharedRef>& blocks) const
{
    return ScheduleEncode(DataPartCount_, ParityPartCount_, WordSize_, Schedule_, blocks);
}

std::vector<TSharedRef> TCauchyReedSolomon::Decode(
    const std::vector<TSharedRef>& blocks,
    const TPartIndexList& erasedIndices) const
{
    if (erasedIndices.empty()) {
        return std::vector<TSharedRef>();
    }

    return BitMatrixDecode(DataPartCount_, ParityPartCount_, WordSize_, BitMatrix_, blocks, erasedIndices);
}

std::optional<TPartIndexList> TCauchyReedSolomon::GetRepairIndices(const TPartIndexList& erasedIndices) const
{
    if (erasedIndices.empty()) {
        return TPartIndexList();
    }

    TPartIndexList indices = erasedIndices;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

    if (indices.size() > ParityPartCount_) {
        return std::nullopt;
    }

    return Difference(0, DataPartCount_ + ParityPartCount_, indices);
}

bool TCauchyReedSolomon::CanRepair(const TPartIndexList& erasedIndices) const
{
    return erasedIndices.size() <= ParityPartCount_;
}

bool TCauchyReedSolomon::CanRepair(const TPartIndexSet& erasedIndices) const
{
    return erasedIndices.count() <= ParityPartCount_;
}

int TCauchyReedSolomon::GetDataPartCount() const
{
    return DataPartCount_;
}

int TCauchyReedSolomon::GetParityPartCount() const
{
    return ParityPartCount_;
}

int TCauchyReedSolomon::GetGuaranteedRepairablePartCount()
{
    return ParityPartCount_;
}

int TCauchyReedSolomon::GetWordSize() const
{
    return WordSize_ * 8;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NErasure
