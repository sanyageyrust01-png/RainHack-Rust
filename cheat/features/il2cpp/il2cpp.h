#pragma once
#include <windows.h>
#include <cstdint>

// IL2CPP API typedefs — resolved at runtime from GameAssembly.dll exports
// For internal cheat: we are inside the process, so we call these directly.

typedef void* Il2CppDomain;
typedef void* Il2CppAssembly;
typedef void* Il2CppImage;
typedef void* Il2CppClass;
typedef void* Il2CppMethodInfo;
typedef void* Il2CppObject;
typedef void* Il2CppFieldInfo;

typedef Il2CppDomain*       (*fn_il2cpp_domain_get)();
typedef Il2CppAssembly**    (*fn_il2cpp_domain_get_assemblies)(Il2CppDomain* domain, size_t* count);
typedef Il2CppImage*        (*fn_il2cpp_assembly_get_image)(Il2CppAssembly* assembly);
typedef const char*         (*fn_il2cpp_image_get_name)(Il2CppImage* image);
typedef Il2CppClass*        (*fn_il2cpp_class_from_name)(Il2CppImage* image, const char* namespaze, const char* name);
typedef Il2CppMethodInfo*   (*fn_il2cpp_class_get_method_from_name)(Il2CppClass* klass, const char* name, int argsCount);
typedef Il2CppFieldInfo*    (*fn_il2cpp_class_get_field_from_name)(Il2CppClass* klass, const char* name);
typedef void*               (*fn_il2cpp_class_get_static_field_data)(Il2CppClass* klass);
typedef void                (*fn_il2cpp_field_static_get_value)(Il2CppFieldInfo* field, void* value);
typedef void                (*fn_il2cpp_field_get_value)(Il2CppObject* obj, Il2CppFieldInfo* field, void* value);
typedef int                 (*fn_il2cpp_field_get_offset)(Il2CppFieldInfo* field);
typedef Il2CppObject*       (*fn_il2cpp_runtime_invoke)(Il2CppMethodInfo* method, void* obj, void** params, void** exc);
typedef void                (*fn_il2cpp_runtime_class_init)(Il2CppClass* klass);
typedef Il2CppClass*        (*fn_il2cpp_object_get_class)(Il2CppObject* obj);
typedef const char*         (*fn_il2cpp_class_get_name)(Il2CppClass* klass);
typedef Il2CppClass*        (*fn_il2cpp_class_get_parent)(Il2CppClass* klass);
typedef void*               (*fn_il2cpp_thread_attach)(Il2CppDomain* domain);
typedef wchar_t*            (*fn_il2cpp_string_chars)(void* str);
typedef int                 (*fn_il2cpp_string_length)(void* str);
typedef Il2CppMethodInfo*   (*fn_il2cpp_class_get_methods)(Il2CppClass* klass, void** iter);
typedef const char*         (*fn_il2cpp_method_get_name)(Il2CppMethodInfo* method);
typedef uint32_t            (*fn_il2cpp_method_get_param_count)(const Il2CppMethodInfo* method);
typedef Il2CppFieldInfo*    (*fn_il2cpp_class_get_fields)(Il2CppClass* klass, void** iter);
typedef const char*         (*fn_il2cpp_field_get_name)(Il2CppFieldInfo* field);
typedef size_t              (*fn_il2cpp_image_get_class_count)(Il2CppImage* image);
typedef Il2CppClass*        (*fn_il2cpp_image_get_class)(Il2CppImage* image, size_t index);
typedef void*               (*fn_il2cpp_class_get_type)(Il2CppClass* klass);
typedef Il2CppObject*       (*fn_il2cpp_type_get_object)(void* type);
typedef uint32_t            (*fn_il2cpp_array_length)(void* arr);
typedef void*               (*fn_il2cpp_resolve_icall)(const char* signature);
typedef void*               (*fn_il2cpp_field_get_type)(Il2CppFieldInfo* field);
typedef const char*         (*fn_il2cpp_type_get_name)(const void* type);
typedef Il2CppClass*        (*fn_il2cpp_class_from_il2cpp_type)(const void* type);
// il2cpp_array_new(elementClass, length) — allocates a managed T[] of the
// given element type and size. Used to build argument arrays for IL methods
// that expect Camera[] etc. Optional because it may not be exported on every
// stripped Unity build, but it usually is.
typedef void*               (*fn_il2cpp_array_new)(Il2CppClass* elementClass, uintptr_t length);
typedef void*               (*fn_il2cpp_string_new)(const char* str);
typedef void*               (*fn_il2cpp_string_new_utf16)(const wchar_t* str, int len);

struct IL2CPP {
    fn_il2cpp_domain_get                    domain_get;
    fn_il2cpp_domain_get_assemblies         domain_get_assemblies;
    fn_il2cpp_assembly_get_image            assembly_get_image;
    fn_il2cpp_image_get_name                image_get_name;
    fn_il2cpp_class_from_name               class_from_name;
    fn_il2cpp_class_get_method_from_name    class_get_method_from_name;
    fn_il2cpp_class_get_field_from_name     class_get_field_from_name;
    fn_il2cpp_class_get_static_field_data   class_get_static_field_data;
    fn_il2cpp_field_static_get_value        field_static_get_value;
    fn_il2cpp_field_get_value               field_get_value;
    fn_il2cpp_field_get_offset              field_get_offset;
    fn_il2cpp_runtime_invoke                runtime_invoke;
    fn_il2cpp_runtime_class_init            runtime_class_init;
    fn_il2cpp_object_get_class              object_get_class;
    fn_il2cpp_class_get_name                class_get_name;
    fn_il2cpp_class_get_parent              class_get_parent;
    fn_il2cpp_thread_attach                 thread_attach;
    fn_il2cpp_string_chars                  string_chars;
    fn_il2cpp_string_length                 string_length;
    fn_il2cpp_class_get_methods             class_get_methods;
    fn_il2cpp_method_get_name               method_get_name;
    fn_il2cpp_method_get_param_count        method_get_param_count;
    fn_il2cpp_class_get_fields              class_get_fields;
    fn_il2cpp_field_get_name                field_get_name;
    fn_il2cpp_image_get_class_count         image_get_class_count;
    fn_il2cpp_image_get_class               image_get_class;
    fn_il2cpp_class_get_type                class_get_type;
    fn_il2cpp_type_get_object               type_get_object;
    fn_il2cpp_array_length                  array_length;
    fn_il2cpp_resolve_icall                 resolve_icall;
    fn_il2cpp_array_new                     array_new;
    fn_il2cpp_string_new                    string_new;
    fn_il2cpp_string_new_utf16              string_new_utf16;
    fn_il2cpp_field_get_type                field_get_type;
    fn_il2cpp_type_get_name                 type_get_name;
    fn_il2cpp_class_from_il2cpp_type        class_from_il2cpp_type;

    bool Init() {
        HMODULE ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) return false;

        #define RESOLVE(name) name = (fn_il2cpp_##name)GetProcAddress(ga, "il2cpp_" #name); if(!name) return false;
        #define RESOLVE_OPT(name) name = (fn_il2cpp_##name)GetProcAddress(ga, "il2cpp_" #name);
        RESOLVE(domain_get);
        RESOLVE(domain_get_assemblies);
        RESOLVE(assembly_get_image);
        RESOLVE(image_get_name);
        RESOLVE(class_from_name);
        RESOLVE(class_get_method_from_name);
        RESOLVE(class_get_field_from_name);
        RESOLVE(class_get_static_field_data);
        RESOLVE(field_static_get_value);
        RESOLVE(field_get_value);
        RESOLVE(field_get_offset);
        RESOLVE(runtime_invoke);
        RESOLVE(runtime_class_init);
        RESOLVE(object_get_class);
        RESOLVE(class_get_name);
        RESOLVE(class_get_parent);
        RESOLVE(thread_attach);
        RESOLVE(string_chars);
        RESOLVE(string_length);
        RESOLVE_OPT(class_get_methods);
        RESOLVE_OPT(method_get_name);
        RESOLVE_OPT(method_get_param_count);
        RESOLVE_OPT(class_get_fields);
        RESOLVE_OPT(field_get_name);
        RESOLVE_OPT(image_get_class_count);
        RESOLVE_OPT(image_get_class);
        RESOLVE_OPT(class_get_type);
        RESOLVE_OPT(type_get_object);
        RESOLVE_OPT(array_length);
        RESOLVE_OPT(resolve_icall);
        RESOLVE_OPT(array_new);
        RESOLVE_OPT(string_new);
        RESOLVE_OPT(string_new_utf16);
        RESOLVE_OPT(field_get_type);
        RESOLVE_OPT(type_get_name);
        RESOLVE_OPT(class_from_il2cpp_type);
        #undef RESOLVE
        #undef RESOLVE_OPT
        return true;
    }

    // Helper: find image by name (e.g. "Assembly-CSharp.dll")
    Il2CppImage* FindImage(const char* imageName) {
        auto domain = domain_get();
        if (!domain) return nullptr;
        size_t count = 0;
        auto assemblies = domain_get_assemblies(domain, &count);
        for (size_t i = 0; i < count; i++) {
            auto image = assembly_get_image(assemblies[i]);
            if (image) {
                const char* name = image_get_name(image);
                if (name && strcmp(name, imageName) == 0)
                    return image;
            }
        }
        return nullptr;
    }
};

inline IL2CPP il2cpp;
