package ru.yandex.yt.ytclient.rpc;

import java.util.HashMap;
import java.util.Map;

public enum RpcMessageType {
    REQUEST(0x69637072),
    CANCEL(0x63637072),
    RESPONSE(0x6f637072);

    private final int value;

    RpcMessageType(int value) {
        this.value = value;
    }

    public int getValue() {
        return value;
    }

    public static RpcMessageType fromValue(int value) {
        RpcMessageType type = types.get(value);
        if (type == null) {
            throw new IllegalArgumentException(
                    "Unsupported rpc message type: 0x" + Integer.toUnsignedString(value, 16));
        }
        return type;
    }

    private static final Map<Integer, RpcMessageType> types = new HashMap<>();

    static {
        for (RpcMessageType type : values()) {
            types.put(type.getValue(), type);
        }
    }
}
