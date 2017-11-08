#include "proto_helpers.h"

#include <mapreduce/yt/interface/io.h>
#include <mapreduce/yt/interface/protos/extension.pb.h>
#include <mapreduce/yt/common/fluent.h>

#include <contrib/libs/protobuf/descriptor.h>
#include <contrib/libs/protobuf/google/protobuf/descriptor.pb.h>
#include <contrib/libs/protobuf/messagext.h>
#include <contrib/libs/protobuf/io/coded_stream.h>

#include <util/stream/str.h>
#include <util/stream/file.h>
#include <util/folder/path.h>

namespace NYT {

using ::google::protobuf::Message;
using ::google::protobuf::Descriptor;
using ::google::protobuf::DescriptorPool;

using ::google::protobuf::io::CodedInputStream;
using ::google::protobuf::io::TCopyingInputStreamAdaptor;

////////////////////////////////////////////////////////////////////////////////

namespace {

yvector<const Descriptor*> GetJobDescriptors(const TString& fileName)
{
    yvector<const Descriptor*> descriptors;
    if (!TFsPath(fileName).Exists()) {
        ythrow TIOException() <<
            "Cannot load '" << fileName << "' file";
    }

    TIFStream input(fileName);
    TString line;
    while (input.ReadLine(line)) {
        const auto* pool = DescriptorPool::generated_pool();
        const auto* descriptor = pool->FindMessageTypeByName(line);
        descriptors.push_back(descriptor);
    }

    return descriptors;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

yvector<const Descriptor*> GetJobInputDescriptors()
{
    return GetJobDescriptors("proto_input");
}

yvector<const Descriptor*> GetJobOutputDescriptors()
{
    return GetJobDescriptors("proto_output");
}

void ValidateProtoDescriptor(
    const Message& row,
    size_t tableIndex,
    const yvector<const Descriptor*>& descriptors,
    bool isRead)
{
    const char* direction = isRead ? "input" : "output";

    if (tableIndex >= descriptors.size()) {
        ythrow TIOException() <<
            "Table index " << tableIndex <<
            " is out of range [0, " << descriptors.size() <<
            ") in " << direction;
    }

    if (row.GetDescriptor() != descriptors[tableIndex]) {
        ythrow TIOException() <<
            "Invalid row of type " << row.GetDescriptor()->full_name() <<
            " at index " << tableIndex <<
            ", row of type " << descriptors[tableIndex]->full_name() <<
            " expected in " << direction;
    }
}

void ParseFromStream(IInputStream* stream, Message& row, ui32 length)
{
    TLengthLimitedInput input(stream, length);
    TCopyingInputStreamAdaptor adaptor(&input);
    CodedInputStream codedStream(&adaptor);
    codedStream.SetTotalBytesLimit(length + 1, length + 1);
    bool parsedOk = row.ParseFromCodedStream(&codedStream);
    Y_ENSURE(parsedOk, "Failed to parse protobuf message");

    Y_ENSURE(input.Left() == 0);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
