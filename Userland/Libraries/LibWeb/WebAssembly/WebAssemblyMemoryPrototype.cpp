/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebAssembly/WebAssemblyMemoryPrototype.h>
#include <LibWeb/WebAssembly/WebAssemblyObject.h>

namespace Web::Bindings {

void WebAssemblyMemoryPrototype::initialize(JS::Realm& realm)
{
    Object::initialize(realm);
    define_native_accessor(realm, "buffer", buffer_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_function(realm, "grow", grow, 1, JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable);
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyMemoryPrototype::grow)
{
    auto page_count = TRY(vm.argument(0).to_u32(vm));
    auto* this_object = TRY(vm.this_value().to_object(vm));
    if (!is<WebAssemblyMemoryObject>(this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "WebAssembly.Memory");
    auto* memory_object = static_cast<WebAssemblyMemoryObject*>(this_object);
    auto address = memory_object->address();
    auto* memory = WebAssemblyObject::s_abstract_machine.store().get(address);
    if (!memory)
        return JS::js_undefined();

    auto previous_size = memory->size() / Wasm::Constants::page_size;
    if (!memory->grow(page_count * Wasm::Constants::page_size))
        return vm.throw_completion<JS::TypeError>("Memory.grow() grows past the stated limit of the memory instance");

    return JS::Value(static_cast<u32>(previous_size));
}

JS_DEFINE_NATIVE_FUNCTION(WebAssemblyMemoryPrototype::buffer_getter)
{
    auto& realm = *vm.current_realm();

    auto* this_object = TRY(vm.this_value().to_object(vm));
    if (!is<WebAssemblyMemoryObject>(this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "WebAssembly.Memory");
    auto* memory_object = static_cast<WebAssemblyMemoryObject*>(this_object);
    auto address = memory_object->address();
    auto* memory = WebAssemblyObject::s_abstract_machine.store().get(address);
    if (!memory)
        return JS::js_undefined();

    auto* array_buffer = JS::ArrayBuffer::create(realm, &memory->data());
    array_buffer->set_detach_key(JS::js_string(vm, "WebAssembly.Memory"));
    return array_buffer;
}

}
