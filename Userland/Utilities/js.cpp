/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteBuffer.h>
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Stream.h>
#include <LibCore/System.h>
#include <LibJS/Bytecode/BasicBlock.h>
#include <LibJS/Bytecode/Generator.h>
#include <LibJS/Bytecode/Interpreter.h>
#include <LibJS/Bytecode/PassManager.h>
#include <LibJS/Console.h>
#include <LibJS/Interpreter.h>
#include <LibJS/Parser.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/AsyncGenerator.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DatePrototype.h>
#include <LibJS/Runtime/ECMAScriptFunctionObject.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GeneratorObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DisplayNames.h>
#include <LibJS/Runtime/Intl/DurationFormat.h>
#include <LibJS/Runtime/Intl/ListFormat.h>
#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/Intl/RelativeTimeFormat.h>
#include <LibJS/Runtime/Intl/Segmenter.h>
#include <LibJS/Runtime/Intl/Segments.h>
#include <LibJS/Runtime/JSONObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/ProxyObject.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/ShadowRealm.h>
#include <LibJS/Runtime/Shape.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/StringPrototype.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/Instant.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/WeakMap.h>
#include <LibJS/Runtime/WeakRef.h>
#include <LibJS/Runtime/WeakSet.h>
#include <LibJS/SourceTextModule.h>
#include <LibLine/Editor.h>
#include <LibMain/Main.h>
#include <LibTextCodec/Decoder.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

RefPtr<JS::VM> g_vm;
Vector<String> g_repl_statements;
JS::Handle<JS::Value> g_last_value = JS::make_handle(JS::js_undefined());

class ReplObject final : public JS::GlobalObject {
    JS_OBJECT(ReplObject, JS::GlobalObject);

public:
    ReplObject(JS::Realm& realm)
        : GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~ReplObject() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(exit_interpreter);
    JS_DECLARE_NATIVE_FUNCTION(repl_help);
    JS_DECLARE_NATIVE_FUNCTION(save_to_file);
    JS_DECLARE_NATIVE_FUNCTION(load_ini);
    JS_DECLARE_NATIVE_FUNCTION(load_json);
    JS_DECLARE_NATIVE_FUNCTION(last_value_getter);
    JS_DECLARE_NATIVE_FUNCTION(print);
};

class ScriptObject final : public JS::GlobalObject {
    JS_OBJECT(ScriptObject, JS::GlobalObject);

public:
    ScriptObject(JS::Realm& realm)
        : JS::GlobalObject(realm)
    {
    }
    virtual void initialize(JS::Realm&) override;
    virtual ~ScriptObject() override = default;

private:
    JS_DECLARE_NATIVE_FUNCTION(load_ini);
    JS_DECLARE_NATIVE_FUNCTION(load_json);
    JS_DECLARE_NATIVE_FUNCTION(print);
};

static bool s_dump_ast = false;
static bool s_run_bytecode = false;
static bool s_opt_bytecode = false;
static bool s_as_module = false;
static bool s_print_last_result = false;
static bool s_strip_ansi = false;
static bool s_disable_source_location_hints = false;
static RefPtr<Line::Editor> s_editor;
static String s_history_path = String::formatted("{}/.js-history", Core::StandardPaths::home_directory());
static int s_repl_line_level = 0;
static bool s_fail_repl = false;

static String prompt_for_level(int level)
{
    static StringBuilder prompt_builder;
    prompt_builder.clear();
    prompt_builder.append("> "sv);

    for (auto i = 0; i < level; ++i)
        prompt_builder.append("    "sv);

    return prompt_builder.build();
}

static String read_next_piece()
{
    StringBuilder piece;

    auto line_level_delta_for_next_line { 0 };

    do {
        auto line_result = s_editor->get_line(prompt_for_level(s_repl_line_level));

        line_level_delta_for_next_line = 0;

        if (line_result.is_error()) {
            s_fail_repl = true;
            return "";
        }

        auto& line = line_result.value();
        s_editor->add_to_history(line);

        piece.append(line);
        piece.append('\n');
        auto lexer = JS::Lexer(line);

        enum {
            NotInLabelOrObjectKey,
            InLabelOrObjectKeyIdentifier,
            InLabelOrObjectKey
        } label_state { NotInLabelOrObjectKey };

        for (JS::Token token = lexer.next(); token.type() != JS::TokenType::Eof; token = lexer.next()) {
            switch (token.type()) {
            case JS::TokenType::BracketOpen:
            case JS::TokenType::CurlyOpen:
            case JS::TokenType::ParenOpen:
                label_state = NotInLabelOrObjectKey;
                s_repl_line_level++;
                break;
            case JS::TokenType::BracketClose:
            case JS::TokenType::CurlyClose:
            case JS::TokenType::ParenClose:
                label_state = NotInLabelOrObjectKey;
                s_repl_line_level--;
                break;

            case JS::TokenType::Identifier:
            case JS::TokenType::StringLiteral:
                if (label_state == NotInLabelOrObjectKey)
                    label_state = InLabelOrObjectKeyIdentifier;
                else
                    label_state = NotInLabelOrObjectKey;
                break;
            case JS::TokenType::Colon:
                if (label_state == InLabelOrObjectKeyIdentifier)
                    label_state = InLabelOrObjectKey;
                else
                    label_state = NotInLabelOrObjectKey;
                break;
            default:
                break;
            }
        }

        if (label_state == InLabelOrObjectKey) {
            // If there's a label or object literal key at the end of this line,
            // prompt for more lines but do not change the line level.
            line_level_delta_for_next_line += 1;
        }
    } while (s_repl_line_level + line_level_delta_for_next_line > 0);

    return piece.to_string();
}

static String strip_ansi(StringView format_string)
{
    if (format_string.is_empty())
        return String::empty();

    StringBuilder builder;
    size_t i;
    for (i = 0; i < format_string.length() - 1; ++i) {
        if (format_string[i] == '\033' && format_string[i + 1] == '[') {
            while (i < format_string.length() && format_string[i] != 'm')
                ++i;
        } else {
            builder.append(format_string[i]);
        }
    }
    if (i < format_string.length())
        builder.append(format_string[i]);
    return builder.to_string();
}

template<typename... Parameters>
static void js_out(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    if (!s_strip_ansi)
        return out(move(fmtstr), parameters...);
    auto stripped_fmtstr = strip_ansi(fmtstr.view());
    out(stripped_fmtstr, parameters...);
}

template<typename... Parameters>
static void js_outln(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    if (!s_strip_ansi)
        return outln(move(fmtstr), parameters...);
    auto stripped_fmtstr = strip_ansi(fmtstr.view());
    outln(stripped_fmtstr, parameters...);
}

inline void js_outln() { outln(); }

static void print_value(JS::Value value, HashTable<JS::Object*>& seen_objects);

static void print_type(FlyString const& name)
{
    js_out("[\033[36;1m{}\033[0m]", name);
}

static void print_separator(bool& first)
{
    js_out(first ? " "sv : ", "sv);
    first = false;
}

static void print_array(JS::Array const& array, HashTable<JS::Object*>& seen_objects)
{
    js_out("[");
    bool first = true;
    for (auto it = array.indexed_properties().begin(false); it != array.indexed_properties().end(); ++it) {
        print_separator(first);
        auto value_or_error = array.get(it.index());
        // The V8 repl doesn't throw an exception here, and instead just
        // prints 'undefined'. We may choose to replicate that behavior in
        // the future, but for now lets just catch the error
        if (value_or_error.is_error())
            return;
        auto value = value_or_error.release_value();
        print_value(value, seen_objects);
    }
    if (!first)
        js_out(" ");
    js_out("]");
}

static void print_object(JS::Object const& object, HashTable<JS::Object*>& seen_objects)
{
    js_out("{{");
    bool first = true;
    for (auto& entry : object.indexed_properties()) {
        print_separator(first);
        js_out("\"\033[33;1m{}\033[0m\": ", entry.index());
        auto value_or_error = object.get(entry.index());
        // The V8 repl doesn't throw an exception here, and instead just
        // prints 'undefined'. We may choose to replicate that behavior in
        // the future, but for now lets just catch the error
        if (value_or_error.is_error())
            return;
        auto value = value_or_error.release_value();
        print_value(value, seen_objects);
    }
    for (auto& it : object.shape().property_table_ordered()) {
        print_separator(first);
        if (it.key.is_string()) {
            js_out("\"\033[33;1m{}\033[0m\": ", it.key.to_display_string());
        } else {
            js_out("[\033[33;1m{}\033[0m]: ", it.key.to_display_string());
        }
        print_value(object.get_direct(it.value.offset), seen_objects);
    }
    if (!first)
        js_out(" ");
    js_out("}}");
}

static void print_function(JS::FunctionObject const& function_object, HashTable<JS::Object*>&)
{
    if (is<JS::ECMAScriptFunctionObject>(function_object)) {
        auto const& ecmascript_function_object = static_cast<JS::ECMAScriptFunctionObject const&>(function_object);
        switch (ecmascript_function_object.kind()) {
        case JS::FunctionKind::Normal:
            print_type("Function");
            break;
        case JS::FunctionKind::Generator:
            print_type("GeneratorFunction");
            break;
        case JS::FunctionKind::Async:
            print_type("AsyncFunction");
            break;
        case JS::FunctionKind::AsyncGenerator:
            print_type("AsyncGeneratorFunction");
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    } else {
        print_type(function_object.class_name());
    }
    if (is<JS::ECMAScriptFunctionObject>(function_object))
        js_out(" {}", static_cast<JS::ECMAScriptFunctionObject const&>(function_object).name());
    else if (is<JS::NativeFunction>(function_object))
        js_out(" {}", static_cast<JS::NativeFunction const&>(function_object).name());
}

static void print_date(JS::Date const& date, HashTable<JS::Object*>&)
{
    print_type("Date");
    js_out(" \033[34;1m{}\033[0m", JS::to_date_string(date.date_value()));
}

static void print_error(JS::Object const& object, HashTable<JS::Object*>& seen_objects)
{
    auto name = object.get_without_side_effects(g_vm->names.name).value_or(JS::js_undefined());
    auto message = object.get_without_side_effects(g_vm->names.message).value_or(JS::js_undefined());
    if (name.is_accessor() || message.is_accessor()) {
        print_value(&object, seen_objects);
    } else {
        auto name_string = name.to_string_without_side_effects();
        auto message_string = message.to_string_without_side_effects();
        print_type(name_string);
        if (!message_string.is_empty())
            js_out(" \033[31;1m{}\033[0m", message_string);
    }
}

static void print_regexp_object(JS::RegExpObject const& regexp_object, HashTable<JS::Object*>&)
{
    print_type("RegExp");
    js_out(" \033[34;1m/{}/{}\033[0m", regexp_object.escape_regexp_pattern(), regexp_object.flags());
}

static void print_proxy_object(JS::ProxyObject const& proxy_object, HashTable<JS::Object*>& seen_objects)
{
    print_type("Proxy");
    js_out("\n  target: ");
    print_value(&proxy_object.target(), seen_objects);
    js_out("\n  handler: ");
    print_value(&proxy_object.handler(), seen_objects);
}

static void print_map(JS::Map const& map, HashTable<JS::Object*>& seen_objects)
{
    print_type("Map");
    js_out(" {{");
    bool first = true;
    for (auto const& entry : map) {
        print_separator(first);
        print_value(entry.key, seen_objects);
        js_out(" => ");
        print_value(entry.value, seen_objects);
    }
    if (!first)
        js_out(" ");
    js_out("}}");
}

static void print_set(JS::Set const& set, HashTable<JS::Object*>& seen_objects)
{
    print_type("Set");
    js_out(" {{");
    bool first = true;
    for (auto const& entry : set) {
        print_separator(first);
        print_value(entry.key, seen_objects);
    }
    if (!first)
        js_out(" ");
    js_out("}}");
}

static void print_weak_map(JS::WeakMap const& weak_map, HashTable<JS::Object*>&)
{
    print_type("WeakMap");
    js_out(" ({})", weak_map.values().size());
    // Note: We could tell you what's actually inside, but not in insertion order.
}

static void print_weak_set(JS::WeakSet const& weak_set, HashTable<JS::Object*>&)
{
    print_type("WeakSet");
    js_out(" ({})", weak_set.values().size());
    // Note: We could tell you what's actually inside, but not in insertion order.
}

static void print_weak_ref(JS::WeakRef const& weak_ref, HashTable<JS::Object*>& seen_objects)
{
    print_type("WeakRef");
    js_out(" ");
    print_value(weak_ref.value().visit([](Empty) -> JS::Value { return JS::js_undefined(); }, [](auto* value) -> JS::Value { return value; }), seen_objects);
}

static void print_promise(JS::Promise const& promise, HashTable<JS::Object*>& seen_objects)
{
    print_type("Promise");
    switch (promise.state()) {
    case JS::Promise::State::Pending:
        js_out("\n  state: ");
        js_out("\033[36;1mPending\033[0m");
        break;
    case JS::Promise::State::Fulfilled:
        js_out("\n  state: ");
        js_out("\033[32;1mFulfilled\033[0m");
        js_out("\n  result: ");
        print_value(promise.result(), seen_objects);
        break;
    case JS::Promise::State::Rejected:
        js_out("\n  state: ");
        js_out("\033[31;1mRejected\033[0m");
        js_out("\n  result: ");
        print_value(promise.result(), seen_objects);
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

static void print_array_buffer(JS::ArrayBuffer const& array_buffer, HashTable<JS::Object*>& seen_objects)
{
    auto& buffer = array_buffer.buffer();
    auto byte_length = array_buffer.byte_length();
    print_type("ArrayBuffer");
    js_out("\n  byteLength: ");
    print_value(JS::Value((double)byte_length), seen_objects);
    if (!byte_length)
        return;
    js_outln();
    for (size_t i = 0; i < byte_length; ++i) {
        js_out("{:02x}", buffer[i]);
        if (i + 1 < byte_length) {
            if ((i + 1) % 32 == 0)
                js_outln();
            else if ((i + 1) % 16 == 0)
                js_out("  ");
            else
                js_out(" ");
        }
    }
}

static void print_shadow_realm(JS::ShadowRealm const&, HashTable<JS::Object*>&)
{
    // Not much we can show here that would be useful. Realm pointer address?!
    print_type("ShadowRealm");
}

static void print_generator(JS::GeneratorObject const&, HashTable<JS::Object*>&)
{
    print_type("Generator");
}

static void print_async_generator(JS::AsyncGenerator const&, HashTable<JS::Object*>&)
{
    print_type("AsyncGenerator");
}

template<typename T>
static void print_number(T number) requires IsArithmetic<T>
{
    js_out("\033[35;1m");
    js_out("{}", number);
    js_out("\033[0m");
}

static void print_typed_array(JS::TypedArrayBase const& typed_array_base, HashTable<JS::Object*>& seen_objects)
{
    auto& array_buffer = *typed_array_base.viewed_array_buffer();
    auto length = typed_array_base.array_length();
    print_type(typed_array_base.class_name());
    js_out("\n  length: ");
    print_value(JS::Value(length), seen_objects);
    js_out("\n  byteLength: ");
    print_value(JS::Value(typed_array_base.byte_length()), seen_objects);
    js_out("\n  buffer: ");
    print_type("ArrayBuffer");
    if (array_buffer.is_detached())
        js_out(" (detached)");
    js_out(" @ {:p}", &array_buffer);
    if (!length || array_buffer.is_detached())
        return;
    js_outln();
    // FIXME: This kinda sucks.
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    if (is<JS::ClassName>(typed_array_base)) {                                           \
        js_out("[ ");                                                                    \
        auto& typed_array = static_cast<JS::ClassName const&>(typed_array_base);         \
        auto data = typed_array.data();                                                  \
        for (size_t i = 0; i < length; ++i) {                                            \
            if (i > 0)                                                                   \
                js_out(", ");                                                            \
            print_number(data[i]);                                                       \
        }                                                                                \
        js_out(" ]");                                                                    \
        return;                                                                          \
    }
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
    VERIFY_NOT_REACHED();
}

static void print_data_view(JS::DataView const& data_view, HashTable<JS::Object*>& seen_objects)
{
    print_type("DataView");
    js_out("\n  byteLength: ");
    print_value(JS::Value(data_view.byte_length()), seen_objects);
    js_out("\n  byteOffset: ");
    print_value(JS::Value(data_view.byte_offset()), seen_objects);
    js_out("\n  buffer: ");
    print_type("ArrayBuffer");
    js_out(" @ {:p}", data_view.viewed_array_buffer());
}

static void print_temporal_calendar(JS::Temporal::Calendar const& calendar, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.Calendar");
    js_out(" ");
    print_value(JS::js_string(calendar.vm(), calendar.identifier()), seen_objects);
}

static void print_temporal_duration(JS::Temporal::Duration const& duration, HashTable<JS::Object*>&)
{
    print_type("Temporal.Duration");
    js_out(" \033[34;1m{} y, {} M, {} w, {} d, {} h, {} m, {} s, {} ms, {} us, {} ns\033[0m", duration.years(), duration.months(), duration.weeks(), duration.days(), duration.hours(), duration.minutes(), duration.seconds(), duration.milliseconds(), duration.microseconds(), duration.nanoseconds());
}

static void print_temporal_instant(JS::Temporal::Instant const& instant, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.Instant");
    js_out(" ");
    // FIXME: Print human readable date and time, like in print_date() - ideally handling arbitrarily large values since we get a bigint.
    print_value(&instant.nanoseconds(), seen_objects);
}

static void print_temporal_plain_date(JS::Temporal::PlainDate const& plain_date, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.PlainDate");
    js_out(" \033[34;1m{:04}-{:02}-{:02}\033[0m", plain_date.iso_year(), plain_date.iso_month(), plain_date.iso_day());
    js_out("\n  calendar: ");
    print_value(&plain_date.calendar(), seen_objects);
}

static void print_temporal_plain_date_time(JS::Temporal::PlainDateTime const& plain_date_time, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.PlainDateTime");
    js_out(" \033[34;1m{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}{:03}{:03}\033[0m", plain_date_time.iso_year(), plain_date_time.iso_month(), plain_date_time.iso_day(), plain_date_time.iso_hour(), plain_date_time.iso_minute(), plain_date_time.iso_second(), plain_date_time.iso_millisecond(), plain_date_time.iso_microsecond(), plain_date_time.iso_nanosecond());
    js_out("\n  calendar: ");
    print_value(&plain_date_time.calendar(), seen_objects);
}

static void print_temporal_plain_month_day(JS::Temporal::PlainMonthDay const& plain_month_day, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.PlainMonthDay");
    // Also has an [[ISOYear]] internal slot, but showing that here seems rather unexpected.
    js_out(" \033[34;1m{:02}-{:02}\033[0m", plain_month_day.iso_month(), plain_month_day.iso_day());
    js_out("\n  calendar: ");
    print_value(&plain_month_day.calendar(), seen_objects);
}

static void print_temporal_plain_time(JS::Temporal::PlainTime const& plain_time, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.PlainTime");
    js_out(" \033[34;1m{:02}:{:02}:{:02}.{:03}{:03}{:03}\033[0m", plain_time.iso_hour(), plain_time.iso_minute(), plain_time.iso_second(), plain_time.iso_millisecond(), plain_time.iso_microsecond(), plain_time.iso_nanosecond());
    js_out("\n  calendar: ");
    print_value(&plain_time.calendar(), seen_objects);
}

static void print_temporal_plain_year_month(JS::Temporal::PlainYearMonth const& plain_year_month, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.PlainYearMonth");
    // Also has an [[ISODay]] internal slot, but showing that here seems rather unexpected.
    js_out(" \033[34;1m{:04}-{:02}\033[0m", plain_year_month.iso_year(), plain_year_month.iso_month());
    js_out("\n  calendar: ");
    print_value(&plain_year_month.calendar(), seen_objects);
}

static void print_temporal_time_zone(JS::Temporal::TimeZone const& time_zone, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.TimeZone");
    js_out(" ");
    print_value(JS::js_string(time_zone.vm(), time_zone.identifier()), seen_objects);
    if (time_zone.offset_nanoseconds().has_value()) {
        js_out("\n  offset (ns): ");
        print_value(JS::Value(*time_zone.offset_nanoseconds()), seen_objects);
    }
}

static void print_temporal_zoned_date_time(JS::Temporal::ZonedDateTime const& zoned_date_time, HashTable<JS::Object*>& seen_objects)
{
    print_type("Temporal.ZonedDateTime");
    js_out("\n  epochNanoseconds: ");
    print_value(&zoned_date_time.nanoseconds(), seen_objects);
    js_out("\n  timeZone: ");
    print_value(&zoned_date_time.time_zone(), seen_objects);
    js_out("\n  calendar: ");
    print_value(&zoned_date_time.calendar(), seen_objects);
}

static void print_intl_display_names(JS::Intl::DisplayNames const& display_names, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.DisplayNames");
    js_out("\n  locale: ");
    print_value(js_string(display_names.vm(), display_names.locale()), seen_objects);
    js_out("\n  type: ");
    print_value(js_string(display_names.vm(), display_names.type_string()), seen_objects);
    js_out("\n  style: ");
    print_value(js_string(display_names.vm(), display_names.style_string()), seen_objects);
    js_out("\n  fallback: ");
    print_value(js_string(display_names.vm(), display_names.fallback_string()), seen_objects);
    if (display_names.has_language_display()) {
        js_out("\n  languageDisplay: ");
        print_value(js_string(display_names.vm(), display_names.language_display_string()), seen_objects);
    }
}

static void print_intl_locale(JS::Intl::Locale const& locale, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.Locale");
    js_out("\n  locale: ");
    print_value(js_string(locale.vm(), locale.locale()), seen_objects);
    if (locale.has_calendar()) {
        js_out("\n  calendar: ");
        print_value(js_string(locale.vm(), locale.calendar()), seen_objects);
    }
    if (locale.has_case_first()) {
        js_out("\n  caseFirst: ");
        print_value(js_string(locale.vm(), locale.case_first()), seen_objects);
    }
    if (locale.has_collation()) {
        js_out("\n  collation: ");
        print_value(js_string(locale.vm(), locale.collation()), seen_objects);
    }
    if (locale.has_hour_cycle()) {
        js_out("\n  hourCycle: ");
        print_value(js_string(locale.vm(), locale.hour_cycle()), seen_objects);
    }
    if (locale.has_numbering_system()) {
        js_out("\n  numberingSystem: ");
        print_value(js_string(locale.vm(), locale.numbering_system()), seen_objects);
    }
    js_out("\n  numeric: ");
    print_value(JS::Value(locale.numeric()), seen_objects);
}

static void print_intl_list_format(JS::Intl::ListFormat const& list_format, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.ListFormat");
    js_out("\n  locale: ");
    print_value(js_string(list_format.vm(), list_format.locale()), seen_objects);
    js_out("\n  type: ");
    print_value(js_string(list_format.vm(), list_format.type_string()), seen_objects);
    js_out("\n  style: ");
    print_value(js_string(list_format.vm(), list_format.style_string()), seen_objects);
}

static void print_intl_number_format(JS::Intl::NumberFormat const& number_format, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.NumberFormat");
    js_out("\n  locale: ");
    print_value(js_string(number_format.vm(), number_format.locale()), seen_objects);
    js_out("\n  dataLocale: ");
    print_value(js_string(number_format.vm(), number_format.data_locale()), seen_objects);
    js_out("\n  numberingSystem: ");
    print_value(js_string(number_format.vm(), number_format.numbering_system()), seen_objects);
    js_out("\n  style: ");
    print_value(js_string(number_format.vm(), number_format.style_string()), seen_objects);
    if (number_format.has_currency()) {
        js_out("\n  currency: ");
        print_value(js_string(number_format.vm(), number_format.currency()), seen_objects);
    }
    if (number_format.has_currency_display()) {
        js_out("\n  currencyDisplay: ");
        print_value(js_string(number_format.vm(), number_format.currency_display_string()), seen_objects);
    }
    if (number_format.has_currency_sign()) {
        js_out("\n  currencySign: ");
        print_value(js_string(number_format.vm(), number_format.currency_sign_string()), seen_objects);
    }
    if (number_format.has_unit()) {
        js_out("\n  unit: ");
        print_value(js_string(number_format.vm(), number_format.unit()), seen_objects);
    }
    if (number_format.has_unit_display()) {
        js_out("\n  unitDisplay: ");
        print_value(js_string(number_format.vm(), number_format.unit_display_string()), seen_objects);
    }
    js_out("\n  minimumIntegerDigits: ");
    print_value(JS::Value(number_format.min_integer_digits()), seen_objects);
    if (number_format.has_min_fraction_digits()) {
        js_out("\n  minimumFractionDigits: ");
        print_value(JS::Value(number_format.min_fraction_digits()), seen_objects);
    }
    if (number_format.has_max_fraction_digits()) {
        js_out("\n  maximumFractionDigits: ");
        print_value(JS::Value(number_format.max_fraction_digits()), seen_objects);
    }
    if (number_format.has_min_significant_digits()) {
        js_out("\n  minimumSignificantDigits: ");
        print_value(JS::Value(number_format.min_significant_digits()), seen_objects);
    }
    if (number_format.has_max_significant_digits()) {
        js_out("\n  maximumSignificantDigits: ");
        print_value(JS::Value(number_format.max_significant_digits()), seen_objects);
    }
    js_out("\n  useGrouping: ");
    print_value(number_format.use_grouping_to_value(number_format.vm()), seen_objects);
    js_out("\n  roundingType: ");
    print_value(js_string(number_format.vm(), number_format.rounding_type_string()), seen_objects);
    js_out("\n  roundingMode: ");
    print_value(js_string(number_format.vm(), number_format.rounding_mode_string()), seen_objects);
    js_out("\n  roundingIncrement: ");
    print_value(JS::Value(number_format.rounding_increment()), seen_objects);
    js_out("\n  notation: ");
    print_value(js_string(number_format.vm(), number_format.notation_string()), seen_objects);
    if (number_format.has_compact_display()) {
        js_out("\n  compactDisplay: ");
        print_value(js_string(number_format.vm(), number_format.compact_display_string()), seen_objects);
    }
    js_out("\n  signDisplay: ");
    print_value(js_string(number_format.vm(), number_format.sign_display_string()), seen_objects);
    js_out("\n  trailingZeroDisplay: ");
    print_value(js_string(number_format.vm(), number_format.trailing_zero_display_string()), seen_objects);
}

static void print_intl_date_time_format(JS::Intl::DateTimeFormat& date_time_format, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.DateTimeFormat");
    js_out("\n  locale: ");
    print_value(js_string(date_time_format.vm(), date_time_format.locale()), seen_objects);
    js_out("\n  pattern: ");
    print_value(js_string(date_time_format.vm(), date_time_format.pattern()), seen_objects);
    js_out("\n  calendar: ");
    print_value(js_string(date_time_format.vm(), date_time_format.calendar()), seen_objects);
    js_out("\n  numberingSystem: ");
    print_value(js_string(date_time_format.vm(), date_time_format.numbering_system()), seen_objects);
    if (date_time_format.has_hour_cycle()) {
        js_out("\n  hourCycle: ");
        print_value(js_string(date_time_format.vm(), date_time_format.hour_cycle_string()), seen_objects);
    }
    js_out("\n  timeZone: ");
    print_value(js_string(date_time_format.vm(), date_time_format.time_zone()), seen_objects);
    if (date_time_format.has_date_style()) {
        js_out("\n  dateStyle: ");
        print_value(js_string(date_time_format.vm(), date_time_format.date_style_string()), seen_objects);
    }
    if (date_time_format.has_time_style()) {
        js_out("\n  timeStyle: ");
        print_value(js_string(date_time_format.vm(), date_time_format.time_style_string()), seen_objects);
    }

    JS::Intl::for_each_calendar_field(date_time_format.vm(), date_time_format, [&](auto& option, auto const& property, auto const&) -> JS::ThrowCompletionOr<void> {
        using ValueType = typename RemoveReference<decltype(option)>::ValueType;

        if (!option.has_value())
            return {};

        js_out("\n  {}: ", property);

        if constexpr (IsIntegral<ValueType>) {
            print_value(JS::Value(*option), seen_objects);
        } else {
            auto name = Locale::calendar_pattern_style_to_string(*option);
            print_value(js_string(date_time_format.vm(), name), seen_objects);
        }

        return {};
    });
}

static void print_intl_relative_time_format(JS::Intl::RelativeTimeFormat const& date_time_format, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.RelativeTimeFormat");
    js_out("\n  locale: ");
    print_value(js_string(date_time_format.vm(), date_time_format.locale()), seen_objects);
    js_out("\n  numberingSystem: ");
    print_value(js_string(date_time_format.vm(), date_time_format.numbering_system()), seen_objects);
    js_out("\n  style: ");
    print_value(js_string(date_time_format.vm(), date_time_format.style_string()), seen_objects);
    js_out("\n  numeric: ");
    print_value(js_string(date_time_format.vm(), date_time_format.numeric_string()), seen_objects);
}

static void print_intl_plural_rules(JS::Intl::PluralRules const& plural_rules, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.PluralRules");
    js_out("\n  locale: ");
    print_value(js_string(plural_rules.vm(), plural_rules.locale()), seen_objects);
    js_out("\n  type: ");
    print_value(js_string(plural_rules.vm(), plural_rules.type_string()), seen_objects);
    js_out("\n  minimumIntegerDigits: ");
    print_value(JS::Value(plural_rules.min_integer_digits()), seen_objects);
    if (plural_rules.has_min_fraction_digits()) {
        js_out("\n  minimumFractionDigits: ");
        print_value(JS::Value(plural_rules.min_fraction_digits()), seen_objects);
    }
    if (plural_rules.has_max_fraction_digits()) {
        js_out("\n  maximumFractionDigits: ");
        print_value(JS::Value(plural_rules.max_fraction_digits()), seen_objects);
    }
    if (plural_rules.has_min_significant_digits()) {
        js_out("\n  minimumSignificantDigits: ");
        print_value(JS::Value(plural_rules.min_significant_digits()), seen_objects);
    }
    if (plural_rules.has_max_significant_digits()) {
        js_out("\n  maximumSignificantDigits: ");
        print_value(JS::Value(plural_rules.max_significant_digits()), seen_objects);
    }
    js_out("\n  roundingType: ");
    print_value(js_string(plural_rules.vm(), plural_rules.rounding_type_string()), seen_objects);
}

static void print_intl_collator(JS::Intl::Collator const& collator, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.Collator");
    out("\n  locale: ");
    print_value(js_string(collator.vm(), collator.locale()), seen_objects);
    out("\n  usage: ");
    print_value(js_string(collator.vm(), collator.usage_string()), seen_objects);
    out("\n  sensitivity: ");
    print_value(js_string(collator.vm(), collator.sensitivity_string()), seen_objects);
    out("\n  caseFirst: ");
    print_value(js_string(collator.vm(), collator.case_first_string()), seen_objects);
    out("\n  collation: ");
    print_value(js_string(collator.vm(), collator.collation()), seen_objects);
    out("\n  ignorePunctuation: ");
    print_value(JS::Value(collator.ignore_punctuation()), seen_objects);
    out("\n  numeric: ");
    print_value(JS::Value(collator.numeric()), seen_objects);
}

static void print_intl_segmenter(JS::Intl::Segmenter const& segmenter, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.Segmenter");
    out("\n  locale: ");
    print_value(js_string(segmenter.vm(), segmenter.locale()), seen_objects);
    out("\n  granularity: ");
    print_value(js_string(segmenter.vm(), segmenter.segmenter_granularity_string()), seen_objects);
}

static void print_intl_segments(JS::Intl::Segments const& segments, HashTable<JS::Object*>& seen_objects)
{
    print_type("Segments");
    out("\n  string: ");
    print_value(js_string(segments.vm(), segments.segments_string()), seen_objects);
    out("\n  segmenter: ");
    print_value(&segments.segments_segmenter(), seen_objects);
}

static void print_intl_duration_format(JS::Intl::DurationFormat const& duration_format, HashTable<JS::Object*>& seen_objects)
{
    print_type("Intl.DurationFormat");
    out("\n  locale: ");
    print_value(js_string(duration_format.vm(), duration_format.locale()), seen_objects);
    out("\n  dataLocale: ");
    print_value(js_string(duration_format.vm(), duration_format.data_locale()), seen_objects);
    out("\n  numberingSystem: ");
    print_value(js_string(duration_format.vm(), duration_format.numbering_system()), seen_objects);
    out("\n  style: ");
    print_value(js_string(duration_format.vm(), duration_format.style_string()), seen_objects);
    out("\n  years: ");
    print_value(js_string(duration_format.vm(), duration_format.years_style_string()), seen_objects);
    out("\n  yearsDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.years_display_string()), seen_objects);
    out("\n  months: ");
    print_value(js_string(duration_format.vm(), duration_format.months_style_string()), seen_objects);
    out("\n  monthsDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.months_display_string()), seen_objects);
    out("\n  weeks: ");
    print_value(js_string(duration_format.vm(), duration_format.weeks_style_string()), seen_objects);
    out("\n  weeksDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.weeks_display_string()), seen_objects);
    out("\n  days: ");
    print_value(js_string(duration_format.vm(), duration_format.days_style_string()), seen_objects);
    out("\n  daysDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.days_display_string()), seen_objects);
    out("\n  hours: ");
    print_value(js_string(duration_format.vm(), duration_format.hours_style_string()), seen_objects);
    out("\n  hoursDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.hours_display_string()), seen_objects);
    out("\n  minutes: ");
    print_value(js_string(duration_format.vm(), duration_format.minutes_style_string()), seen_objects);
    out("\n  minutesDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.minutes_display_string()), seen_objects);
    out("\n  seconds: ");
    print_value(js_string(duration_format.vm(), duration_format.seconds_style_string()), seen_objects);
    out("\n  secondsDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.seconds_display_string()), seen_objects);
    out("\n  milliseconds: ");
    print_value(js_string(duration_format.vm(), duration_format.milliseconds_style_string()), seen_objects);
    out("\n  millisecondsDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.milliseconds_display_string()), seen_objects);
    out("\n  microseconds: ");
    print_value(js_string(duration_format.vm(), duration_format.microseconds_style_string()), seen_objects);
    out("\n  microsecondsDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.microseconds_display_string()), seen_objects);
    out("\n  nanoseconds: ");
    print_value(js_string(duration_format.vm(), duration_format.nanoseconds_style_string()), seen_objects);
    out("\n  nanosecondsDisplay: ");
    print_value(js_string(duration_format.vm(), duration_format.nanoseconds_display_string()), seen_objects);
    if (duration_format.has_fractional_digits()) {
        out("\n  fractionalDigits: ");
        print_value(JS::Value(duration_format.fractional_digits()), seen_objects);
    }
}

static void print_boolean_object(JS::BooleanObject const& boolean_object, HashTable<JS::Object*>& seen_objects)
{
    print_type("Boolean");
    js_out(" ");
    print_value(JS::Value(boolean_object.boolean()), seen_objects);
}

static void print_number_object(JS::NumberObject const& number_object, HashTable<JS::Object*>& seen_objects)
{
    print_type("Number");
    js_out(" ");
    print_value(JS::Value(number_object.number()), seen_objects);
}

static void print_string_object(JS::StringObject const& string_object, HashTable<JS::Object*>& seen_objects)
{
    print_type("String");
    js_out(" ");
    print_value(&string_object.primitive_string(), seen_objects);
}

static void print_value(JS::Value value, HashTable<JS::Object*>& seen_objects)
{
    if (value.is_empty()) {
        js_out("\033[34;1m<empty>\033[0m");
        return;
    }

    if (value.is_object()) {
        if (seen_objects.contains(&value.as_object())) {
            // FIXME: Maybe we should only do this for circular references,
            //        not for all reoccurring objects.
            js_out("<already printed Object {}>", &value.as_object());
            return;
        }
        seen_objects.set(&value.as_object());
    }

    if (value.is_object()) {
        auto& object = value.as_object();
        if (is<JS::Array>(object))
            return print_array(static_cast<JS::Array&>(object), seen_objects);
        if (object.is_function())
            return print_function(static_cast<JS::FunctionObject&>(object), seen_objects);
        if (is<JS::Date>(object))
            return print_date(static_cast<JS::Date&>(object), seen_objects);
        if (is<JS::Error>(object))
            return print_error(object, seen_objects);

        auto prototype_or_error = object.internal_get_prototype_of();
        if (prototype_or_error.has_value() && prototype_or_error.value() != nullptr) {
            auto& prototype = *prototype_or_error.value();
            if (&prototype == prototype.shape().realm().intrinsics().error_prototype())
                return print_error(object, seen_objects);
        }

        if (is<JS::RegExpObject>(object))
            return print_regexp_object(static_cast<JS::RegExpObject&>(object), seen_objects);
        if (is<JS::Map>(object))
            return print_map(static_cast<JS::Map&>(object), seen_objects);
        if (is<JS::Set>(object))
            return print_set(static_cast<JS::Set&>(object), seen_objects);
        if (is<JS::WeakMap>(object))
            return print_weak_map(static_cast<JS::WeakMap&>(object), seen_objects);
        if (is<JS::WeakSet>(object))
            return print_weak_set(static_cast<JS::WeakSet&>(object), seen_objects);
        if (is<JS::WeakRef>(object))
            return print_weak_ref(static_cast<JS::WeakRef&>(object), seen_objects);
        if (is<JS::DataView>(object))
            return print_data_view(static_cast<JS::DataView&>(object), seen_objects);
        if (is<JS::ProxyObject>(object))
            return print_proxy_object(static_cast<JS::ProxyObject&>(object), seen_objects);
        if (is<JS::Promise>(object))
            return print_promise(static_cast<JS::Promise&>(object), seen_objects);
        if (is<JS::ArrayBuffer>(object))
            return print_array_buffer(static_cast<JS::ArrayBuffer&>(object), seen_objects);
        if (is<JS::ShadowRealm>(object))
            return print_shadow_realm(static_cast<JS::ShadowRealm&>(object), seen_objects);
        if (is<JS::GeneratorObject>(object))
            return print_generator(static_cast<JS::GeneratorObject&>(object), seen_objects);
        if (is<JS::AsyncGenerator>(object))
            return print_async_generator(static_cast<JS::AsyncGenerator&>(object), seen_objects);
        if (object.is_typed_array())
            return print_typed_array(static_cast<JS::TypedArrayBase&>(object), seen_objects);
        if (is<JS::BooleanObject>(object))
            return print_boolean_object(static_cast<JS::BooleanObject&>(object), seen_objects);
        if (is<JS::NumberObject>(object))
            return print_number_object(static_cast<JS::NumberObject&>(object), seen_objects);
        if (is<JS::StringObject>(object))
            return print_string_object(static_cast<JS::StringObject&>(object), seen_objects);
        if (is<JS::Temporal::Calendar>(object))
            return print_temporal_calendar(static_cast<JS::Temporal::Calendar&>(object), seen_objects);
        if (is<JS::Temporal::Duration>(object))
            return print_temporal_duration(static_cast<JS::Temporal::Duration&>(object), seen_objects);
        if (is<JS::Temporal::Instant>(object))
            return print_temporal_instant(static_cast<JS::Temporal::Instant&>(object), seen_objects);
        if (is<JS::Temporal::PlainDate>(object))
            return print_temporal_plain_date(static_cast<JS::Temporal::PlainDate&>(object), seen_objects);
        if (is<JS::Temporal::PlainDateTime>(object))
            return print_temporal_plain_date_time(static_cast<JS::Temporal::PlainDateTime&>(object), seen_objects);
        if (is<JS::Temporal::PlainMonthDay>(object))
            return print_temporal_plain_month_day(static_cast<JS::Temporal::PlainMonthDay&>(object), seen_objects);
        if (is<JS::Temporal::PlainTime>(object))
            return print_temporal_plain_time(static_cast<JS::Temporal::PlainTime&>(object), seen_objects);
        if (is<JS::Temporal::PlainYearMonth>(object))
            return print_temporal_plain_year_month(static_cast<JS::Temporal::PlainYearMonth&>(object), seen_objects);
        if (is<JS::Temporal::TimeZone>(object))
            return print_temporal_time_zone(static_cast<JS::Temporal::TimeZone&>(object), seen_objects);
        if (is<JS::Temporal::ZonedDateTime>(object))
            return print_temporal_zoned_date_time(static_cast<JS::Temporal::ZonedDateTime&>(object), seen_objects);
        if (is<JS::Intl::DisplayNames>(object))
            return print_intl_display_names(static_cast<JS::Intl::DisplayNames&>(object), seen_objects);
        if (is<JS::Intl::Locale>(object))
            return print_intl_locale(static_cast<JS::Intl::Locale&>(object), seen_objects);
        if (is<JS::Intl::ListFormat>(object))
            return print_intl_list_format(static_cast<JS::Intl::ListFormat&>(object), seen_objects);
        if (is<JS::Intl::NumberFormat>(object))
            return print_intl_number_format(static_cast<JS::Intl::NumberFormat&>(object), seen_objects);
        if (is<JS::Intl::DateTimeFormat>(object))
            return print_intl_date_time_format(static_cast<JS::Intl::DateTimeFormat&>(object), seen_objects);
        if (is<JS::Intl::RelativeTimeFormat>(object))
            return print_intl_relative_time_format(static_cast<JS::Intl::RelativeTimeFormat&>(object), seen_objects);
        if (is<JS::Intl::PluralRules>(object))
            return print_intl_plural_rules(static_cast<JS::Intl::PluralRules&>(object), seen_objects);
        if (is<JS::Intl::Collator>(object))
            return print_intl_collator(static_cast<JS::Intl::Collator&>(object), seen_objects);
        if (is<JS::Intl::Segmenter>(object))
            return print_intl_segmenter(static_cast<JS::Intl::Segmenter&>(object), seen_objects);
        if (is<JS::Intl::Segments>(object))
            return print_intl_segments(static_cast<JS::Intl::Segments&>(object), seen_objects);
        if (is<JS::Intl::DurationFormat>(object))
            return print_intl_duration_format(static_cast<JS::Intl::DurationFormat&>(object), seen_objects);
        return print_object(object, seen_objects);
    }

    if (value.is_string())
        js_out("\033[32;1m");
    else if (value.is_number() || value.is_bigint())
        js_out("\033[35;1m");
    else if (value.is_boolean())
        js_out("\033[33;1m");
    else if (value.is_null())
        js_out("\033[33;1m");
    else if (value.is_undefined())
        js_out("\033[34;1m");
    if (value.is_string())
        js_out("\"");
    else if (value.is_negative_zero())
        js_out("-");
    js_out("{}", value.to_string_without_side_effects());
    if (value.is_string())
        js_out("\"");
    js_out("\033[0m");
}

static void print(JS::Value value)
{
    HashTable<JS::Object*> seen_objects;
    print_value(value, seen_objects);
    js_outln();
}

static bool write_to_file(String const& path)
{
    int fd = open(path.characters(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    for (size_t i = 0; i < g_repl_statements.size(); i++) {
        auto line = g_repl_statements[i];
        if (line.length() && i != g_repl_statements.size() - 1) {
            ssize_t nwritten = write(fd, line.characters(), line.length());
            if (nwritten < 0) {
                close(fd);
                return false;
            }
        }
        if (i != g_repl_statements.size() - 1) {
            char ch = '\n';
            ssize_t nwritten = write(fd, &ch, 1);
            if (nwritten != 1) {
                perror("write");
                close(fd);
                return false;
            }
        }
    }
    close(fd);
    return true;
}

static bool parse_and_run(JS::Interpreter& interpreter, StringView source, StringView source_name)
{
    enum class ReturnEarly {
        No,
        Yes,
    };

    JS::ThrowCompletionOr<JS::Value> result { JS::js_undefined() };

    auto run_script_or_module = [&](auto& script_or_module) {
        if (s_dump_ast)
            script_or_module->parse_node().dump(0);

        if (JS::Bytecode::g_dump_bytecode || s_run_bytecode) {
            auto executable_result = JS::Bytecode::Generator::generate(script_or_module->parse_node());
            if (executable_result.is_error()) {
                result = g_vm->throw_completion<JS::InternalError>(executable_result.error().to_string());
                return ReturnEarly::No;
            }

            auto executable = executable_result.release_value();
            executable->name = source_name;
            if (s_opt_bytecode) {
                auto& passes = JS::Bytecode::Interpreter::optimization_pipeline();
                passes.perform(*executable);
                dbgln("Optimisation passes took {}us", passes.elapsed());
            }

            if (JS::Bytecode::g_dump_bytecode)
                executable->dump();

            if (s_run_bytecode) {
                JS::Bytecode::Interpreter bytecode_interpreter(interpreter.realm());
                auto result_or_error = bytecode_interpreter.run_and_return_frame(*executable, nullptr);
                if (result_or_error.value.is_error())
                    result = result_or_error.value.release_error();
                else
                    result = result_or_error.frame->registers[0];
            } else {
                return ReturnEarly::Yes;
            }
        } else {
            result = interpreter.run(*script_or_module);
        }

        return ReturnEarly::No;
    };

    if (!s_as_module) {
        auto script_or_error = JS::Script::parse(source, interpreter.realm(), source_name);
        if (script_or_error.is_error()) {
            auto error = script_or_error.error()[0];
            auto hint = error.source_location_hint(source);
            if (!hint.is_empty())
                outln("{}", hint);
            outln("{}", error.to_string());
            result = interpreter.vm().throw_completion<JS::SyntaxError>(error.to_string());
        } else {
            auto return_early = run_script_or_module(script_or_error.value());
            if (return_early == ReturnEarly::Yes)
                return true;
        }
    } else {
        auto module_or_error = JS::SourceTextModule::parse(source, interpreter.realm(), source_name);
        if (module_or_error.is_error()) {
            auto error = module_or_error.error()[0];
            auto hint = error.source_location_hint(source);
            if (!hint.is_empty())
                outln("{}", hint);
            outln(error.to_string());
            result = interpreter.vm().throw_completion<JS::SyntaxError>(error.to_string());
        } else {
            auto return_early = run_script_or_module(module_or_error.value());
            if (return_early == ReturnEarly::Yes)
                return true;
        }
    }

    auto handle_exception = [&](JS::Value thrown_value) {
        js_out("Uncaught exception: ");
        print(thrown_value);

        if (!thrown_value.is_object() || !is<JS::Error>(thrown_value.as_object()))
            return;
        auto& traceback = static_cast<JS::Error const&>(thrown_value.as_object()).traceback();
        if (traceback.size() > 1) {
            unsigned repetitions = 0;
            for (size_t i = 0; i < traceback.size(); ++i) {
                auto& traceback_frame = traceback[i];
                if (i + 1 < traceback.size()) {
                    auto& next_traceback_frame = traceback[i + 1];
                    if (next_traceback_frame.function_name == traceback_frame.function_name) {
                        repetitions++;
                        continue;
                    }
                }
                if (repetitions > 4) {
                    // If more than 5 (1 + >4) consecutive function calls with the same name, print
                    // the name only once and show the number of repetitions instead. This prevents
                    // printing ridiculously large call stacks of recursive functions.
                    js_outln(" -> {}", traceback_frame.function_name);
                    js_outln(" {} more calls", repetitions);
                } else {
                    for (size_t j = 0; j < repetitions + 1; ++j)
                        js_outln(" -> {}", traceback_frame.function_name);
                }
                repetitions = 0;
            }
        }
    };

    if (!result.is_error())
        g_last_value = JS::make_handle(result.value());

    if (result.is_error()) {
        VERIFY(result.throw_completion().value().has_value());
        handle_exception(*result.release_error().value());
        return false;
    } else if (s_print_last_result) {
        print(result.value());
    }
    return true;
}

static JS::ThrowCompletionOr<JS::Value> load_ini_impl(JS::VM& vm)
{
    auto& realm = *vm.current_realm();

    auto filename = TRY(vm.argument(0).to_string(vm));
    auto file_or_error = Core::Stream::File::open(filename, Core::Stream::OpenMode::Read);
    if (file_or_error.is_error())
        return vm.throw_completion<JS::Error>(String::formatted("Failed to open '{}': {}", filename, file_or_error.error()));

    auto config_file = MUST(Core::ConfigFile::open(filename, file_or_error.release_value()));
    auto* object = JS::Object::create(realm, realm.intrinsics().object_prototype());
    for (auto const& group : config_file->groups()) {
        auto* group_object = JS::Object::create(realm, realm.intrinsics().object_prototype());
        for (auto const& key : config_file->keys(group)) {
            auto entry = config_file->read_entry(group, key);
            group_object->define_direct_property(key, js_string(vm, move(entry)), JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);
        }
        object->define_direct_property(group, group_object, JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable);
    }
    return object;
}

static JS::ThrowCompletionOr<JS::Value> load_json_impl(JS::VM& vm)
{
    auto filename = TRY(vm.argument(0).to_string(vm));
    auto file_or_error = Core::Stream::File::open(filename, Core::Stream::OpenMode::Read);
    if (file_or_error.is_error())
        return vm.throw_completion<JS::Error>(String::formatted("Failed to open '{}': {}", filename, file_or_error.error()));

    auto file_contents_or_error = file_or_error.value()->read_all();
    if (file_contents_or_error.is_error())
        return vm.throw_completion<JS::Error>(String::formatted("Failed to read '{}': {}", filename, file_contents_or_error.error()));

    auto json = JsonValue::from_string(file_contents_or_error.value());
    if (json.is_error())
        return vm.throw_completion<JS::SyntaxError>(JS::ErrorType::JsonMalformed);
    return JS::JSONObject::parse_json_value(vm, json.value());
}

void ReplObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global", this, JS::Attribute::Enumerable);
    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "exit", exit_interpreter, 0, attr);
    define_native_function(realm, "help", repl_help, 0, attr);
    define_native_function(realm, "save", save_to_file, 1, attr);
    define_native_function(realm, "loadINI", load_ini, 1, attr);
    define_native_function(realm, "loadJSON", load_json, 1, attr);
    define_native_function(realm, "print", print, 1, attr);

    define_native_accessor(
        realm,
        "_",
        [](JS::VM&) {
            return g_last_value.value();
        },
        [](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
            auto& global_object = vm.get_global_object();
            VERIFY(is<ReplObject>(global_object));
            outln("Disable writing last value to '_'");

            // We must delete first otherwise this setter gets called recursively.
            TRY(global_object.internal_delete(JS::PropertyKey { "_" }));

            auto value = vm.argument(0);
            TRY(global_object.internal_set(JS::PropertyKey { "_" }, value, &global_object));
            return value;
        },
        attr);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::save_to_file)
{
    if (!vm.argument_count())
        return JS::Value(false);
    String save_path = vm.argument(0).to_string_without_side_effects();
    if (write_to_file(save_path)) {
        return JS::Value(true);
    }
    return JS::Value(false);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::exit_interpreter)
{
    if (!vm.argument_count())
        exit(0);
    exit(TRY(vm.argument(0).to_number(vm)).as_double());
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::repl_help)
{
    js_outln("REPL commands:");
    js_outln("    exit(code): exit the REPL with specified code. Defaults to 0.");
    js_outln("    help(): display this menu");
    js_outln("    loadINI(file): load the given file as INI.");
    js_outln("    loadJSON(file): load the given file as JSON.");
    js_outln("    print(value): pretty-print the given JS value.");
    js_outln("    save(file): write REPL input history to the given file. For example: save(\"foo.txt\")");
    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::load_ini)
{
    return load_ini_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::load_json)
{
    return load_json_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ReplObject::print)
{
    ::print(vm.argument(0));
    return JS::js_undefined();
}

void ScriptObject::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_direct_property("global", this, JS::Attribute::Enumerable);
    u8 attr = JS::Attribute::Configurable | JS::Attribute::Writable | JS::Attribute::Enumerable;
    define_native_function(realm, "loadINI", load_ini, 1, attr);
    define_native_function(realm, "loadJSON", load_json, 1, attr);
    define_native_function(realm, "print", print, 1, attr);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::load_ini)
{
    return load_ini_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::load_json)
{
    return load_json_impl(vm);
}

JS_DEFINE_NATIVE_FUNCTION(ScriptObject::print)
{
    ::print(vm.argument(0));
    return JS::js_undefined();
}

static void repl(JS::Interpreter& interpreter)
{
    while (!s_fail_repl) {
        String piece = read_next_piece();
        if (Utf8View { piece }.trim(JS::whitespace_characters).is_empty())
            continue;

        g_repl_statements.append(piece);
        parse_and_run(interpreter, piece, "REPL"sv);
    }
}

static Function<void()> interrupt_interpreter;
static void sigint_handler()
{
    interrupt_interpreter();
}

class ReplConsoleClient final : public JS::ConsoleClient {
public:
    ReplConsoleClient(JS::Console& console)
        : ConsoleClient(console)
    {
    }

    virtual void clear() override
    {
        js_out("\033[3J\033[H\033[2J");
        m_group_stack_depth = 0;
        fflush(stdout);
    }

    virtual void end_group() override
    {
        if (m_group_stack_depth > 0)
            m_group_stack_depth--;
    }

    // 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
    virtual JS::ThrowCompletionOr<JS::Value> printer(JS::Console::LogLevel log_level, PrinterArguments arguments) override
    {
        String indent = String::repeated("  "sv, m_group_stack_depth);

        if (log_level == JS::Console::LogLevel::Trace) {
            auto trace = arguments.get<JS::Console::Trace>();
            StringBuilder builder;
            if (!trace.label.is_empty())
                builder.appendff("{}\033[36;1m{}\033[0m\n", indent, trace.label);

            for (auto& function_name : trace.stack)
                builder.appendff("{}-> {}\n", indent, function_name);

            js_outln("{}", builder.string_view());
            return JS::js_undefined();
        }

        if (log_level == JS::Console::LogLevel::Group || log_level == JS::Console::LogLevel::GroupCollapsed) {
            auto group = arguments.get<JS::Console::Group>();
            js_outln("{}\033[36;1m{}\033[0m", indent, group.label);
            m_group_stack_depth++;
            return JS::js_undefined();
        }

        auto output = String::join(' ', arguments.get<JS::MarkedVector<JS::Value>>());
#ifdef AK_OS_SERENITY
        m_console.output_debug_message(log_level, output);
#endif

        switch (log_level) {
        case JS::Console::LogLevel::Debug:
            js_outln("{}\033[36;1m{}\033[0m", indent, output);
            break;
        case JS::Console::LogLevel::Error:
        case JS::Console::LogLevel::Assert:
            js_outln("{}\033[31;1m{}\033[0m", indent, output);
            break;
        case JS::Console::LogLevel::Info:
            js_outln("{}(i) {}", indent, output);
            break;
        case JS::Console::LogLevel::Log:
            js_outln("{}{}", indent, output);
            break;
        case JS::Console::LogLevel::Warn:
        case JS::Console::LogLevel::CountReset:
            js_outln("{}\033[33;1m{}\033[0m", indent, output);
            break;
        default:
            js_outln("{}{}", indent, output);
            break;
        }
        return JS::js_undefined();
    }

private:
    int m_group_stack_depth { 0 };
};

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    TRY(Core::System::pledge("stdio rpath wpath cpath tty sigaction"));

    bool gc_on_every_allocation = false;
    bool disable_syntax_highlight = false;
    StringView evaluate_script;
    Vector<StringView> script_paths;

    Core::ArgsParser args_parser;
    args_parser.set_general_help("This is a JavaScript interpreter.");
    args_parser.add_option(s_dump_ast, "Dump the AST", "dump-ast", 'A');
    args_parser.add_option(JS::Bytecode::g_dump_bytecode, "Dump the bytecode", "dump-bytecode", 'd');
    args_parser.add_option(s_run_bytecode, "Run the bytecode", "run-bytecode", 'b');
    args_parser.add_option(s_opt_bytecode, "Optimize the bytecode", "optimize-bytecode", 'p');
    args_parser.add_option(s_as_module, "Treat as module", "as-module", 'm');
    args_parser.add_option(s_print_last_result, "Print last result", "print-last-result", 'l');
    args_parser.add_option(s_strip_ansi, "Disable ANSI colors", "disable-ansi-colors", 'i');
    args_parser.add_option(s_disable_source_location_hints, "Disable source location hints", "disable-source-location-hints", 'h');
    args_parser.add_option(gc_on_every_allocation, "GC on every allocation", "gc-on-every-allocation", 'g');
    args_parser.add_option(disable_syntax_highlight, "Disable live syntax highlighting", "no-syntax-highlight", 's');
    args_parser.add_option(evaluate_script, "Evaluate argument as a script", "evaluate", 'c', "script");
    args_parser.add_positional_argument(script_paths, "Path to script files", "scripts", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    bool syntax_highlight = !disable_syntax_highlight;

    g_vm = JS::VM::create();
    g_vm->enable_default_host_import_module_dynamically_hook();

    // NOTE: These will print out both warnings when using something like Promise.reject().catch(...) -
    // which is, as far as I can tell, correct - a promise is created, rejected without handler, and a
    // handler then attached to it. The Node.js REPL doesn't warn in this case, so it's something we
    // might want to revisit at a later point and disable warnings for promises created this way.
    g_vm->on_promise_unhandled_rejection = [](auto& promise) {
        // FIXME: Optionally make print_value() to print to stderr
        js_out("WARNING: A promise was rejected without any handlers");
        js_out(" (result: ");
        HashTable<JS::Object*> seen_objects;
        print_value(promise.result(), seen_objects);
        js_outln(")");
    };
    g_vm->on_promise_rejection_handled = [](auto& promise) {
        // FIXME: Optionally make print_value() to print to stderr
        js_out("WARNING: A handler was added to an already rejected promise");
        js_out(" (result: ");
        HashTable<JS::Object*> seen_objects;
        print_value(promise.result(), seen_objects);
        js_outln(")");
    };
    OwnPtr<JS::Interpreter> interpreter;

    // FIXME: Figure out some way to interrupt the interpreter now that vm.exception() is gone.

    if (evaluate_script.is_empty() && script_paths.is_empty()) {
        s_print_last_result = true;
        interpreter = JS::Interpreter::create<ReplObject>(*g_vm);
        auto& console_object = *interpreter->realm().intrinsics().console_object();
        ReplConsoleClient console_client(console_object.console());
        console_object.console().set_client(console_client);
        interpreter->heap().set_should_collect_on_every_allocation(gc_on_every_allocation);

        auto& global_environment = interpreter->realm().global_environment();

        s_editor = Line::Editor::construct();
        s_editor->load_history(s_history_path);

        signal(SIGINT, [](int) {
            if (!s_editor->is_editing())
                sigint_handler();
            s_editor->save_history(s_history_path);
        });

        s_editor->on_display_refresh = [syntax_highlight](Line::Editor& editor) {
            auto stylize = [&](Line::Span span, Line::Style styles) {
                if (syntax_highlight)
                    editor.stylize(span, styles);
            };
            editor.strip_styles();

            size_t open_indents = s_repl_line_level;

            auto line = editor.line();
            JS::Lexer lexer(line);
            bool indenters_starting_line = true;
            for (JS::Token token = lexer.next(); token.type() != JS::TokenType::Eof; token = lexer.next()) {
                auto length = Utf8View { token.value() }.length();
                auto start = token.offset();
                auto end = start + length;
                if (indenters_starting_line) {
                    if (token.type() != JS::TokenType::ParenClose && token.type() != JS::TokenType::BracketClose && token.type() != JS::TokenType::CurlyClose) {
                        indenters_starting_line = false;
                    } else {
                        --open_indents;
                    }
                }

                switch (token.category()) {
                case JS::TokenCategory::Invalid:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Red), Line::Style::Underline });
                    break;
                case JS::TokenCategory::Number:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Magenta) });
                    break;
                case JS::TokenCategory::String:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Green), Line::Style::Bold });
                    break;
                case JS::TokenCategory::Punctuation:
                    break;
                case JS::TokenCategory::Operator:
                    break;
                case JS::TokenCategory::Keyword:
                    switch (token.type()) {
                    case JS::TokenType::BoolLiteral:
                    case JS::TokenType::NullLiteral:
                        stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Yellow), Line::Style::Bold });
                        break;
                    default:
                        stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Blue), Line::Style::Bold });
                        break;
                    }
                    break;
                case JS::TokenCategory::ControlKeyword:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::Cyan), Line::Style::Italic });
                    break;
                case JS::TokenCategory::Identifier:
                    stylize({ start, end, Line::Span::CodepointOriented }, { Line::Style::Foreground(Line::Style::XtermColor::White), Line::Style::Bold });
                    break;
                default:
                    break;
                }
            }

            editor.set_prompt(prompt_for_level(open_indents));
        };

        auto complete = [&interpreter, &global_environment](Line::Editor const& editor) -> Vector<Line::CompletionSuggestion> {
            auto line = editor.line(editor.cursor());

            JS::Lexer lexer { line };
            enum {
                Initial,
                CompleteVariable,
                CompleteNullProperty,
                CompleteProperty,
            } mode { Initial };

            StringView variable_name;
            StringView property_name;

            // we're only going to complete either
            //    - <N>
            //        where N is part of the name of a variable
            //    - <N>.<P>
            //        where N is the complete name of a variable and
            //        P is part of the name of one of its properties
            auto js_token = lexer.next();
            for (; js_token.type() != JS::TokenType::Eof; js_token = lexer.next()) {
                switch (mode) {
                case CompleteVariable:
                    switch (js_token.type()) {
                    case JS::TokenType::Period:
                        // ...<name> <dot>
                        mode = CompleteNullProperty;
                        break;
                    default:
                        // not a dot, reset back to initial
                        mode = Initial;
                        break;
                    }
                    break;
                case CompleteNullProperty:
                    if (js_token.is_identifier_name()) {
                        // ...<name> <dot> <name>
                        mode = CompleteProperty;
                        property_name = js_token.value();
                    } else {
                        mode = Initial;
                    }
                    break;
                case CompleteProperty:
                    // something came after the property access, reset to initial
                case Initial:
                    if (js_token.type() == JS::TokenType::Identifier) {
                        // ...<name>...
                        mode = CompleteVariable;
                        variable_name = js_token.value();
                    } else {
                        mode = Initial;
                    }
                    break;
                }
            }

            bool last_token_has_trivia = js_token.trivia().length() > 0;

            if (mode == CompleteNullProperty) {
                mode = CompleteProperty;
                property_name = ""sv;
                last_token_has_trivia = false; // <name> <dot> [tab] is sensible to complete.
            }

            if (mode == Initial || last_token_has_trivia)
                return {}; // we do not know how to complete this

            Vector<Line::CompletionSuggestion> results;

            Function<void(JS::Shape const&, StringView)> list_all_properties = [&results, &list_all_properties](JS::Shape const& shape, auto property_pattern) {
                for (auto const& descriptor : shape.property_table()) {
                    if (!descriptor.key.is_string())
                        continue;
                    auto key = descriptor.key.as_string();
                    if (key.view().starts_with(property_pattern)) {
                        Line::CompletionSuggestion completion { key, Line::CompletionSuggestion::ForSearch };
                        if (!results.contains_slow(completion)) { // hide duplicates
                            results.append(String(key));
                            results.last().invariant_offset = property_pattern.length();
                        }
                    }
                }
                if (auto const* prototype = shape.prototype()) {
                    list_all_properties(prototype->shape(), property_pattern);
                }
            };

            switch (mode) {
            case CompleteProperty: {
                auto reference_or_error = g_vm->resolve_binding(variable_name, &global_environment);
                if (reference_or_error.is_error())
                    return {};
                auto value_or_error = reference_or_error.value().get_value(*g_vm);
                if (value_or_error.is_error())
                    return {};
                auto variable = value_or_error.value();
                VERIFY(!variable.is_empty());

                if (!variable.is_object())
                    break;

                auto const* object = MUST(variable.to_object(*g_vm));
                auto const& shape = object->shape();
                list_all_properties(shape, property_name);
                break;
            }
            case CompleteVariable: {
                auto const& variable = interpreter->realm().global_object();
                list_all_properties(variable.shape(), variable_name);

                for (auto const& name : global_environment.declarative_record().bindings()) {
                    if (name.starts_with(variable_name)) {
                        results.empend(name);
                        results.last().invariant_offset = variable_name.length();
                    }
                }

                break;
            }
            default:
                VERIFY_NOT_REACHED();
            }

            return results;
        };
        s_editor->on_tab_complete = move(complete);
        repl(*interpreter);
        s_editor->save_history(s_history_path);
    } else {
        interpreter = JS::Interpreter::create<ScriptObject>(*g_vm);
        auto& console_object = *interpreter->realm().intrinsics().console_object();
        ReplConsoleClient console_client(console_object.console());
        console_object.console().set_client(console_client);
        interpreter->heap().set_should_collect_on_every_allocation(gc_on_every_allocation);

        signal(SIGINT, [](int) {
            sigint_handler();
        });

        StringBuilder builder;
        StringView source_name;

        if (evaluate_script.is_empty()) {
            if (script_paths.size() > 1)
                warnln("Warning: Multiple files supplied, this will concatenate the sources and resolve modules as if it was the first file");

            for (auto& path : script_paths) {
                auto file = TRY(Core::Stream::File::open(path, Core::Stream::OpenMode::Read));
                auto file_contents = TRY(file->read_all());
                auto source = StringView { file_contents };

                if (Utf8View { file_contents }.validate()) {
                    builder.append(source);
                } else {
                    auto* decoder = TextCodec::decoder_for("windows-1252");
                    VERIFY(decoder);

                    auto utf8_source = TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark(*decoder, source);
                    builder.append(utf8_source);
                }
            }

            source_name = script_paths[0];
        } else {
            builder.append(evaluate_script);
            source_name = "eval"sv;
        }

        // We resolve modules as if it is the first file

        if (!parse_and_run(*interpreter, builder.string_view(), source_name))
            return 1;
    }

    return 0;
}
