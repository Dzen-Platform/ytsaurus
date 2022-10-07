package ru.yandex.yt.ytclient.rpc;

/**
 * Интерфейс позволяет отменить запрос раньше срока
 */
public interface RpcClientRequestControl {
    /**
     * Отменяет запрос, возвращает true, если запрос был отменён
     */
    boolean cancel();
}
