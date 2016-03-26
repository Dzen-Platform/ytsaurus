#include "framework.h"

#include "column_format_ut.h"

#include <yt/ytlib/table_chunk_format/boolean_column_writer.h>
#include <yt/ytlib/table_chunk_format/boolean_column_reader.h>

#include <yt/ytlib/table_chunk_format/public.h>

namespace NYT {
namespace NTableChunkFormat {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

class TUnversionedBooleanColumnTest
    : public TUnversionedColumnTestBase<bool>
{
protected:

    std::vector<TNullable<bool>> CreateDirectDense()
    {
        std::vector<TNullable<bool>> data;
        for (int i = 0; i < 100 * 100; ++i) {
            data.push_back(i % 2 == 0);
        }
        return data;
    }

    std::vector<TNullable<bool>> CreateDirectRLE()
    {
        std::vector<TNullable<bool>> data;
        for (int i = 0; i < 100; ++i) {
            for (int j = 0; j < 100; ++j) {
                data.push_back(i % 2 == 0);
            }
        }
        return data;
    }

        void Write(IValueColumnWriter* columnWriter)
    {
        WriteSegment(columnWriter, CreateDirectDense());
        WriteSegment(columnWriter, CreateDirectRLE());
    }

    virtual std::unique_ptr<IUnversionedColumnReader> CreateColumnReader() override
    {
        return CreateUnversionedBooleanColumnReader(ColumnMeta_, ColumnIndex, ColumnId);
    }

    virtual std::unique_ptr<IValueColumnWriter> CreateColumnWriter(TDataBlockWriter* blockWriter) override
    {
        return CreateUnversionedBooleanColumnWriter(
            ColumnIndex,
            blockWriter);
    }
};

TEST_F(TUnversionedBooleanColumnTest, ReadValues)
{
    std::vector<TNullable<bool>> expected;
    AppendVector(&expected, CreateDirectDense());
    AppendVector(&expected, CreateDirectRLE());
    
    Validate(CreateRows(expected), 1111, 15555);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableChunkFormat
} // namespace NYT
