// Code generated by protoc-gen-go. DO NOT EDIT.
// versions:
// 	protoc-gen-go v1.28.1
// 	protoc        v3.15.8
// source: yt_proto/yt/core/misc/proto/bloom_filter.proto

package misc

import (
	protoreflect "google.golang.org/protobuf/reflect/protoreflect"
	protoimpl "google.golang.org/protobuf/runtime/protoimpl"
	reflect "reflect"
	sync "sync"
)

const (
	// Verify that this generated code is sufficiently up-to-date.
	_ = protoimpl.EnforceVersion(20 - protoimpl.MinVersion)
	// Verify that runtime/protoimpl is sufficiently up-to-date.
	_ = protoimpl.EnforceVersion(protoimpl.MaxVersion - 20)
)

type TBloomFilter struct {
	state         protoimpl.MessageState
	sizeCache     protoimpl.SizeCache
	unknownFields protoimpl.UnknownFields

	Version   *int32 `protobuf:"varint,1,req,name=version" json:"version,omitempty"`
	Bitmap    []byte `protobuf:"bytes,2,req,name=bitmap" json:"bitmap,omitempty"`
	HashCount *int32 `protobuf:"varint,3,req,name=hash_count,json=hashCount" json:"hash_count,omitempty"`
}

func (x *TBloomFilter) Reset() {
	*x = TBloomFilter{}
	if protoimpl.UnsafeEnabled {
		mi := &file_yt_proto_yt_core_misc_proto_bloom_filter_proto_msgTypes[0]
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		ms.StoreMessageInfo(mi)
	}
}

func (x *TBloomFilter) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*TBloomFilter) ProtoMessage() {}

func (x *TBloomFilter) ProtoReflect() protoreflect.Message {
	mi := &file_yt_proto_yt_core_misc_proto_bloom_filter_proto_msgTypes[0]
	if protoimpl.UnsafeEnabled && x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use TBloomFilter.ProtoReflect.Descriptor instead.
func (*TBloomFilter) Descriptor() ([]byte, []int) {
	return file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescGZIP(), []int{0}
}

func (x *TBloomFilter) GetVersion() int32 {
	if x != nil && x.Version != nil {
		return *x.Version
	}
	return 0
}

func (x *TBloomFilter) GetBitmap() []byte {
	if x != nil {
		return x.Bitmap
	}
	return nil
}

func (x *TBloomFilter) GetHashCount() int32 {
	if x != nil && x.HashCount != nil {
		return *x.HashCount
	}
	return 0
}

var File_yt_proto_yt_core_misc_proto_bloom_filter_proto protoreflect.FileDescriptor

var file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDesc = []byte{
	0x0a, 0x2e, 0x79, 0x74, 0x5f, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x2f, 0x79, 0x74, 0x2f, 0x63, 0x6f,
	0x72, 0x65, 0x2f, 0x6d, 0x69, 0x73, 0x63, 0x2f, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x2f, 0x62, 0x6c,
	0x6f, 0x6f, 0x6d, 0x5f, 0x66, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x2e, 0x70, 0x72, 0x6f, 0x74, 0x6f,
	0x12, 0x0a, 0x4e, 0x59, 0x54, 0x2e, 0x4e, 0x50, 0x72, 0x6f, 0x74, 0x6f, 0x22, 0x5f, 0x0a, 0x0c,
	0x54, 0x42, 0x6c, 0x6f, 0x6f, 0x6d, 0x46, 0x69, 0x6c, 0x74, 0x65, 0x72, 0x12, 0x18, 0x0a, 0x07,
	0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x18, 0x01, 0x20, 0x02, 0x28, 0x05, 0x52, 0x07, 0x76,
	0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x12, 0x16, 0x0a, 0x06, 0x62, 0x69, 0x74, 0x6d, 0x61, 0x70,
	0x18, 0x02, 0x20, 0x02, 0x28, 0x0c, 0x52, 0x06, 0x62, 0x69, 0x74, 0x6d, 0x61, 0x70, 0x12, 0x1d,
	0x0a, 0x0a, 0x68, 0x61, 0x73, 0x68, 0x5f, 0x63, 0x6f, 0x75, 0x6e, 0x74, 0x18, 0x03, 0x20, 0x02,
	0x28, 0x05, 0x52, 0x09, 0x68, 0x61, 0x73, 0x68, 0x43, 0x6f, 0x75, 0x6e, 0x74, 0x42, 0x25, 0x5a,
	0x23, 0x79, 0x74, 0x73, 0x61, 0x75, 0x72, 0x75, 0x73, 0x2e, 0x74, 0x65, 0x63, 0x68, 0x2f, 0x79,
	0x74, 0x2f, 0x67, 0x6f, 0x2f, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x2f, 0x63, 0x6f, 0x72, 0x65, 0x2f,
	0x6d, 0x69, 0x73, 0x63,
}

var (
	file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescOnce sync.Once
	file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescData = file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDesc
)

func file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescGZIP() []byte {
	file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescOnce.Do(func() {
		file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescData = protoimpl.X.CompressGZIP(file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescData)
	})
	return file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDescData
}

var file_yt_proto_yt_core_misc_proto_bloom_filter_proto_msgTypes = make([]protoimpl.MessageInfo, 1)
var file_yt_proto_yt_core_misc_proto_bloom_filter_proto_goTypes = []interface{}{
	(*TBloomFilter)(nil), // 0: NYT.NProto.TBloomFilter
}
var file_yt_proto_yt_core_misc_proto_bloom_filter_proto_depIdxs = []int32{
	0, // [0:0] is the sub-list for method output_type
	0, // [0:0] is the sub-list for method input_type
	0, // [0:0] is the sub-list for extension type_name
	0, // [0:0] is the sub-list for extension extendee
	0, // [0:0] is the sub-list for field type_name
}

func init() { file_yt_proto_yt_core_misc_proto_bloom_filter_proto_init() }
func file_yt_proto_yt_core_misc_proto_bloom_filter_proto_init() {
	if File_yt_proto_yt_core_misc_proto_bloom_filter_proto != nil {
		return
	}
	if !protoimpl.UnsafeEnabled {
		file_yt_proto_yt_core_misc_proto_bloom_filter_proto_msgTypes[0].Exporter = func(v interface{}, i int) interface{} {
			switch v := v.(*TBloomFilter); i {
			case 0:
				return &v.state
			case 1:
				return &v.sizeCache
			case 2:
				return &v.unknownFields
			default:
				return nil
			}
		}
	}
	type x struct{}
	out := protoimpl.TypeBuilder{
		File: protoimpl.DescBuilder{
			GoPackagePath: reflect.TypeOf(x{}).PkgPath(),
			RawDescriptor: file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDesc,
			NumEnums:      0,
			NumMessages:   1,
			NumExtensions: 0,
			NumServices:   0,
		},
		GoTypes:           file_yt_proto_yt_core_misc_proto_bloom_filter_proto_goTypes,
		DependencyIndexes: file_yt_proto_yt_core_misc_proto_bloom_filter_proto_depIdxs,
		MessageInfos:      file_yt_proto_yt_core_misc_proto_bloom_filter_proto_msgTypes,
	}.Build()
	File_yt_proto_yt_core_misc_proto_bloom_filter_proto = out.File
	file_yt_proto_yt_core_misc_proto_bloom_filter_proto_rawDesc = nil
	file_yt_proto_yt_core_misc_proto_bloom_filter_proto_goTypes = nil
	file_yt_proto_yt_core_misc_proto_bloom_filter_proto_depIdxs = nil
}