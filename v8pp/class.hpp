#ifndef V8PP_CLASS_HPP_INCLUDED
#define V8PP_CLASS_HPP_INCLUDED

#include <algorithm>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "v8pp/config.hpp"
#include "v8pp/factory.hpp"
#include "v8pp/function.hpp"
#include "v8pp/persistent.hpp"
#include "v8pp/property.hpp"

namespace v8pp {

template<typename T>
class class_;

namespace detail {

class class_info
{
public:
	using type_index = unsigned;

	class_info(type_index type)
		: type_(type)
	{
	}

	class_info(class_info const&) = delete;
	class_info& operator=(class_info const&) = delete;

	using cast_function = void* (*)(void* ptr);

	void add_base(class_info* info, cast_function cast)
	{
		auto it = std::find_if(bases_.begin(), bases_.end(),
			[info](base_class_info const& base) { return base.info == info; });
		if (it != bases_.end())
		{
			assert(false && "duplicated inheritance");
			throw std::runtime_error("duplicated base class");
		}
		bases_.emplace_back(info, cast);
		info->derivatives_.emplace_back(this);
	}

	bool cast(void*& ptr, type_index type) const
	{
		if (type == type_)
		{
			return true;
		}

		// fast way - search a direct parent
		for (base_class_info const& base : bases_)
		{
			if (base.info->type_ == type)
			{
				ptr = base.cast(ptr);
				return true;
			}
		}

		// slower way - walk on hierarhy
		for (base_class_info const& base : bases_)
		{
			void* p = base.cast(ptr);
			if (base.info->cast(p, type))
			{
				ptr = p;
				return true;
			}
		}

		return false;
	}

	template<typename T>
	void add_object(v8::Isolate* isolate, T* object, v8::Local<v8::Object> handle)
	{
		assert(objects_.find(object) == objects_.end() && "duplicate object");
		objects_.emplace(object, persistent<v8::Object>(isolate, handle));
	}

	template<typename T>
	void remove_object(v8::Isolate* isolate, T* object, void (*destroy)(v8::Isolate* isolate, T* obj))
	{
		auto it = objects_.find(object);
		assert(objects_.find(object) != objects_.end() && "no object");
		if (it != objects_.end())
		{
			it->second.Reset();
			if (destroy)
			{
				destroy(isolate, object);
			}
			objects_.erase(it);
		}
	}

	template<typename T>
	void remove_objects(v8::Isolate* isolate, void (*destroy)(v8::Isolate* isolate, T* obj))
	{
		for (auto& object : objects_)
		{
			object.second.Reset();
			if (destroy)
			{
				destroy(isolate, static_cast<T*>(object.first));
			}
		}
		objects_.clear();
	}

	v8::Local<v8::Object> find_object(v8::Isolate* isolate, void const* object) const
	{
		auto it = objects_.find(const_cast<void*>(object));
		if (it != objects_.end())
		{
			return to_local(isolate, it->second);
		}

		v8::Local<v8::Object> result;
		for (class_info const* info : derivatives_)
		{
			result = info->find_object(isolate, object);
			if (!result.IsEmpty()) break;
		}
		return result;
	}

protected:
	static type_index register_class()
	{
		static type_index next_index = 0;
		return next_index++;
	}

private:
	struct base_class_info
	{
		class_info* info;
		cast_function cast;

		base_class_info(class_info* info, cast_function cast)
			: info(info)
			, cast(cast)
		{
		}
	};

	type_index const type_;
	std::vector<base_class_info> bases_;
	std::vector<class_info*> derivatives_;

	std::unordered_map<void*, persistent<v8::Object>> objects_;
};

template<typename T>
class class_singleton : public class_info
{
	static type_index class_type()
	{
		static type_index const my_type = class_info::register_class();
		return my_type;
	}
private:
	explicit class_singleton(v8::Isolate* isolate, type_index type)
		: class_info(type)
		, isolate_(isolate)
		, ctor_(nullptr)
	{
		v8::Local<v8::FunctionTemplate> func = v8::FunctionTemplate::New(isolate_);
		v8::Local<v8::FunctionTemplate> js_func = v8::FunctionTemplate::New(isolate_,
			[](v8::FunctionCallbackInfo<v8::Value> const& args)
			{
				v8::Isolate* isolate = args.GetIsolate();
				try
				{
					return args.GetReturnValue().Set(instance(isolate).wrap_object(args));
				}
				catch (std::exception const& ex)
				{
					args.GetReturnValue().Set(throw_ex(isolate, ex.what()));
				}
			});

		func_.Reset(isolate_, func);
		js_func_.Reset(isolate_, js_func);

		// each JavaScript instance has 2 internal fields:
		//  0 - pointer to a wrapped C++ object
		//  1 - pointer to the class_singleton
		func->InstanceTemplate()->SetInternalFieldCount(2);
	}

public:
	static class_singleton& instance(v8::Isolate* isolate)
	{
		// Get pointer to singleton instances from v8::Isolate
		using singleton_instances = std::vector<void*>;

		singleton_instances* singletons =
			static_cast<singleton_instances*>(isolate->GetData(V8PP_ISOLATE_DATA_SLOT));
		if (!singletons)
		{
			// No singletons map yet, create and store it
			singletons = new singleton_instances;
			isolate->SetData(V8PP_ISOLATE_DATA_SLOT, singletons);
		}

		// Get singleton instance from the the list by class_type
		type_index const my_type = class_type();
		class_singleton* result;
		if (my_type < singletons->size())
		{
			result = static_cast<class_singleton*>((*singletons)[my_type]);
		}
		else
		{
			// No singleton instance, create and add it
			result = new class_singleton(isolate, my_type);
			singletons->emplace_back(result);
		}
		return *result;
	}

	v8::Isolate* isolate() { return isolate_; }

	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return to_local(isolate_, func_);
	}

	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return to_local(isolate_, js_func_.IsEmpty()? func_ : js_func_);
	}

	template<typename ...Args>
	void ctor()
	{
		ctor_ = [](v8::FunctionCallbackInfo<v8::Value> const& args)
		{
			using ctor_type = T* (*)(v8::Isolate* isolate, Args...);
			return call_from_v8(static_cast<ctor_type>(&factory<T>::create), args);
		};
		class_function_template()->Inherit(js_function_template());
	}

	template<typename U>
	void inherit()
	{
		class_singleton<U>* base = &class_singleton<U>::instance(isolate_);
		add_base(base, [](void* ptr) -> void*
			{
				return static_cast<U*>(static_cast<T*>(ptr));
			});
		js_function_template()->Inherit(base->class_function_template());
	}

	v8::Handle<v8::Object> wrap_external_object(T* wrap)
	{
		v8::EscapableHandleScope scope(isolate_);

		v8::Local<v8::Object> obj = class_function_template()->GetFunction()->NewInstance();
		obj->SetAlignedPointerInInternalField(0, wrap);
		obj->SetAlignedPointerInInternalField(1, this);

		class_info::add_object(isolate_, wrap, obj);

		return scope.Escape(obj);
	}

	v8::Handle<v8::Object> wrap_object(T* wrap)
	{
		v8::EscapableHandleScope scope(isolate_);

		v8::Local<v8::Object> obj = wrap_external_object(wrap);

		v8::Persistent<v8::Object> pobj(isolate_, obj);
		pobj.SetWeak(wrap,
			[](v8::WeakCallbackData<v8::Object, T> const& data)
			{
				instance(data.GetIsolate()).destroy_object(data.GetParameter());
			});

		return scope.Escape(obj);
	}

	v8::Handle<v8::Object> wrap_object(v8::FunctionCallbackInfo<v8::Value> const& args)
	{
		return ctor_? wrap_object(ctor_(args)) : throw std::runtime_error("create is not allowed");
	}

	T* unwrap_object(v8::Local<v8::Value> value)
	{
		v8::HandleScope scope(isolate_);

		while (value->IsObject())
		{
			v8::Handle<v8::Object> obj = value->ToObject();
			if (obj->InternalFieldCount() == 2)
			{
				void* ptr = obj->GetAlignedPointerFromInternalField(0);
				class_info* info = static_cast<class_info*>(obj->GetAlignedPointerFromInternalField(1));
				if (info && info->cast(ptr, class_type()))
				{
					return static_cast<T*>(ptr);
				}
			}
			value = obj->GetPrototype();
		}
		return nullptr;
	}

	v8::Handle<v8::Object> find_object(T const* obj)
	{
		return class_info::find_object(isolate_, obj);
	}

	void destroy_objects()
	{
		class_info::remove_objects(isolate_, &factory<T>::destroy);
	}

	void destroy_object(T* obj)
	{
		class_info::remove_object(isolate_, obj, &factory<T>::destroy);
	}

private:
	v8::Isolate* isolate_;
	std::function<T* (v8::FunctionCallbackInfo<v8::Value> const& args)> ctor_;

	v8::UniquePersistent<v8::FunctionTemplate> func_;
	v8::UniquePersistent<v8::FunctionTemplate> js_func_;
};

} // namespace detail

/// Interface for registering C++ classes in V8
template<typename T>
class class_
{
	using class_singleton = detail::class_singleton<T>;
public:
	explicit class_(v8::Isolate* isolate)
		: class_singleton_(class_singleton::instance(isolate))
	{
	}

	/// Set class constructor signature
	template<typename ...Args>
	class_& ctor()
	{
		class_singleton_.template ctor<Args...>();
		return *this;
	}

	/// Inhert from C++ class U
	template<typename U>
	class_& inherit()
	{
		static_assert(std::is_base_of<U, T>::value, "Class U should be base for class T");
		//TODO: std::is_convertible<T*, U*> and check for duplicates in hierarchy?
		class_singleton_.template inherit<U>();
		return *this;
	}

	/// Set C++ class member function
	template<typename Method>
	typename std::enable_if<
		std::is_member_function_pointer<Method>::value, class_&>::type
	set(char const *name, Method mem_func)
	{
		class_singleton_.class_function_template()->PrototypeTemplate()->Set(
			isolate(), name, wrap_function_template(isolate(), mem_func));
		return *this;
	}

	/// Set static class function
	template<typename Function>
	typename std::enable_if<
		detail::is_function_pointer<Function>::value, class_&>::type
	set(char const *name, Function func)
	{
		class_singleton_.js_function_template()->Set(isolate(), name,
			wrap_function_template(isolate(), func));
		return *this;
	}

	/// Set class member data
	template<typename Attribute>
	typename std::enable_if<
		std::is_member_object_pointer<Attribute>::value, class_&>::type
	set(char const *name, Attribute attribute, bool readonly = false)
	{
		v8::HandleScope scope(isolate());

		v8::AccessorGetterCallback getter = &member_get<Attribute>;
		v8::AccessorSetterCallback setter = &member_set<Attribute>;
		if (readonly)
		{
			setter = nullptr;
		}

		v8::Handle<v8::Value> data = detail::set_external_data(isolate(), attribute);
		v8::PropertyAttribute const prop_attrs = v8::PropertyAttribute(v8::DontDelete | (setter? 0 : v8::ReadOnly));

		class_singleton_.class_function_template()->PrototypeTemplate()->SetAccessor(
			v8pp::to_v8(isolate(), name), getter, setter, data, v8::DEFAULT, prop_attrs);
		return *this;
	}

	/// Set class attribute with getter and setter
	template<typename GetMethod, typename SetMethod>
	typename std::enable_if<std::is_member_function_pointer<GetMethod>::value
		&& std::is_member_function_pointer<SetMethod>::value, class_&>::type
	set(char const *name, property_<GetMethod, SetMethod> prop)
	{
		v8::HandleScope scope(isolate());

		v8::AccessorGetterCallback getter = property_<GetMethod, SetMethod>::get;
		v8::AccessorSetterCallback setter = property_<GetMethod, SetMethod>::set;
		if (property_<GetMethod, SetMethod>::is_readonly)
		{
			setter = nullptr;
		}

		v8::Handle<v8::Value> data = detail::set_external_data(isolate(), prop);
		v8::PropertyAttribute const prop_attrs = v8::PropertyAttribute(v8::DontDelete | (setter? 0 : v8::ReadOnly));

		class_singleton_.class_function_template()->PrototypeTemplate()->SetAccessor(v8pp::to_v8(isolate(), name),
			getter, setter, data, v8::DEFAULT, prop_attrs);
		return *this;
	}

	/// Set value as a read-only property
	template<typename Value>
	class_& set_const(char const* name, Value value)
	{
		v8::HandleScope scope(isolate());

		class_singleton_.class_function_template()->PrototypeTemplate()->Set(v8pp::to_v8(isolate(), name),
			to_v8(isolate(), value), v8::PropertyAttribute(v8::ReadOnly | v8::DontDelete));
		return *this;
	}

	/// v8::Isolate where the class bindings belongs
	v8::Isolate* isolate() { return class_singleton_.isolate(); }

	v8::Local<v8::FunctionTemplate> class_function_template()
	{
		return class_singleton_.class_function_template();
	}

	v8::Local<v8::FunctionTemplate> js_function_template()
	{
		return class_singleton_.js_function_template();
	}

	/// Create JavaScript object which references externally created C++ class.
	/// It will not take ownership of the C++ pointer.
	static v8::Handle<v8::Object> reference_external(v8::Isolate* isolate, T* ext)
	{
		return class_singleton::instance(isolate).wrap_external_object(ext);
	}

	/// As reference_external but delete memory for C++ object
	/// when JavaScript object is deleted. You must use "new" to allocate ext.
	static v8::Handle<v8::Object> import_external(v8::Isolate* isolate, T* ext)
	{
		return class_singleton::instance(isolate).wrap_object(ext);
	}

	/// Get wrapped object from V8 value, may return nullptr on fail.
	static T* unwrap_object(v8::Isolate* isolate, v8::Handle<v8::Value> value)
	{
		return class_singleton::instance(isolate).unwrap_object(value);
	}

	/// Find V8 object handle for a wrapped C++ object, may return empty handle on fail.
	static v8::Handle<v8::Object> find_object(v8::Isolate* isolate, T const* obj)
	{
		return class_singleton::instance(isolate).find_object(obj);
	}

	/// Destroy wrapped C++ object
	static void destroy_object(v8::Isolate* isolate, T* obj)
	{
		class_singleton::instance(isolate).destroy_object(obj);
	}

	/// Destroy all wrapped C++ objects of this class
	static void destroy_objects(v8::Isolate* isolate)
	{
		class_singleton::instance(isolate).destroy_objects();
	}

private:
	template<typename Attribute>
	static void member_get(v8::Local<v8::String>, v8::PropertyCallbackInfo<v8::Value> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		T const& self = v8pp::from_v8<T const&>(isolate, info.This());
		Attribute attr = detail::get_external_data<Attribute>(info.Data());
		info.GetReturnValue().Set(to_v8(isolate, self.*attr));
	}

	template<typename Attribute>
	static void member_set(v8::Local<v8::String>, v8::Local<v8::Value> value, v8::PropertyCallbackInfo<void> const& info)
	{
		v8::Isolate* isolate = info.GetIsolate();

		T& self = v8pp::from_v8<T&>(isolate, info.This());
		Attribute ptr = detail::get_external_data<Attribute>(info.Data());
		using attr_type = typename detail::function_traits<Attribute>::return_type;
		self.*ptr = v8pp::from_v8<attr_type>(isolate, value);
	}

	detail::class_singleton<T>& class_singleton_;
};

} // namespace v8pp

#endif // V8PP_CLASS_HPP_INCLUDED
