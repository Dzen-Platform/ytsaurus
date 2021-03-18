package ru.yandex.type_info;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStreamReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;
import org.opentest4j.AssertionFailedError;

import static org.hamcrest.CoreMatchers.containsString;
import static org.hamcrest.MatcherAssert.assertThat;

public class CommonDataTest {
    @Test
    public void testGoodTypes() {
        var records = parseResource("good-types.txt", 2);
        for (var r : records) {
            var typeYson = r.get(0);
            var typeString = r.get(1);
            var context = String.format("type: %s\nyson: %s\n", typeString, typeYson);
            try {
                var type = TypeIO.parseYson(typeYson);
                Assertions.assertEquals(typeString, type.toString(), context);

                var yson2 = TypeIO.serializeToTextYson(type);
                TiType type2;
                try {
                    type2 = TypeIO.parseYson(yson2);
                } catch (Throwable t) {
                    throw new RuntimeException(String.format("Unexpected parsing error on yson: %s", yson2), t);
                }
                Assertions.assertEquals(type, type2, context);
            } catch (AssertionFailedError t) {
                throw t;
            } catch (Throwable t) {
                throw new RuntimeException(String.format("Unexpected error in %s", context), t);
            }
        }
    }

    @Test
    public void testBadTypes() {
        var records = parseResource("bad-types.txt", 3);
        for (var r : records) {
            var typeYson = r.get(0);
            var exceptionMessage = r.get(1);
            var context = String.format("exception: %s\nyson: %s\n", exceptionMessage, typeYson);
            var t = Assertions.assertThrows(Throwable.class, () -> TypeIO.parseYson(typeYson), context);
            assertThat(context, decapitalize(t.getMessage()), containsString(exceptionMessage));
        }
    }

    static String decapitalize(String text) {
        if (text.isEmpty()) {
            return text;
        }
        return Character.toLowerCase(text.charAt(0)) + text.substring(1);
    }

    private List<List<String>> parseResource(String name, int expectedFieldCount) {
        var resourceStream = getClass().getResourceAsStream(name);

        if (resourceStream == null) {
            throw new RuntimeException("Resource \"" + name + "\" not found");
        }
        String data;
        try {
            var sb = new StringBuilder();
            var reader = new BufferedReader(new InputStreamReader(resourceStream));
            while (true) {
                var line = reader.readLine();
                if (line == null) {
                    break;
                }
                if (line.strip().startsWith("#")) {
                    continue;
                }
                sb.append(line);
            }
            data = sb.toString();
        } catch (IOException e) {
            throw new RuntimeException("Error reading \"" + name + "\"", e);
        }
        var records = data.split(" *;; *", -1);
        var result = new ArrayList<List<String>>();
        for (var r : records) {
            r = r.strip();
            if (r.isEmpty()) {
                continue;
            }
            var fields = r.split(" *:: *", -1);
            if (fields.length != expectedFieldCount) {
                throw new RuntimeException(
                        String.format("Record \"%s\" has unexpected field count; expected: %d; actual: %d",
                                r,
                                expectedFieldCount,
                                fields.length
                        ));
            }
            result.add(Arrays.asList(fields));
        }
        return result;
    }
}
