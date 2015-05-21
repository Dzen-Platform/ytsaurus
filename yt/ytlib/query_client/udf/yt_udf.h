#include <stdlib.h>
#include <stdint.h>

typedef enum EValueType
{
    Min = 0x00,
    TheBottom = 0x01,
    Null = 0x02,
    Int64 = 0x03,
    Uint64 = 0x04,
    Double = 0x05,
    Boolean = 0x06,
    String = 0x10,
    Any = 0x11,
    Max = 0xef
} EValueType;

typedef union TUnversionedValueData
{
    int64_t Int64;
    uint64_t Uint64;
    double Double;
    int8_t Boolean;
    const char* String;
} TUnversionedValueData;

typedef struct TUnversionedValue
{
    int16_t Id;
    int16_t Type;
    int32_t Length;
    TUnversionedValueData Data;
} TUnversionedValue;

typedef struct TExecutionContext TExecutionContext;

char* AllocatePermanentBytes(TExecutionContext* context, size_t size);

char* AllocateBytes(TExecutionContext* context, size_t size);
