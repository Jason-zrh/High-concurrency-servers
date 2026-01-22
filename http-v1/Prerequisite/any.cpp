#include <iostream>
#include <vector>
#include <cassert>
#include <typeinfo>

class Any
{
public:
    Any() 
    :_content(nullptr)
    { }

    template<class T>
    Any(const T& val) 
    :_content(new PlaceHolder<T>(val)) // !!!!! 在这里: PlaceHolder = Holder! !!!!!!!
    { }

    // 拷贝构造
    Any(const Any& other) 
    :_content(other._content ? other._content->clone() : nullptr)
    { }

    ~Any()
    { 
        delete _content;
    }

    Any& swap(Any& other)
    {
        std::swap(_content, other._content);
        return *this;
    }
    // 获取子类对象中保存数据的指针
    template<class T>
    T* Get()
    { 
        assert(typeid(T) == _content->type());
        return &((PlaceHolder<T>*)_content)->_val;
    }

    // 赋值运算符重载
    template<class T>
    Any& operator=(const T& val)
    { 
        // 创建临时对象，交换以后自动销毁
        Any(val).swap(*this);
        return *this;
    }

    Any& operator=(const Any& other)
    { 
        Any(other).swap(*this);
        return *this;
    }


private:
    // 父类
    class Holder
    {
    public:
        virtual ~Holder() { }
        virtual const std::type_info& type() = 0; // 纯虚函数, 必须要在子类中实现
        virtual Holder* clone() = 0;
    };  
    // 子类
    template <class T>
    class PlaceHolder : public Holder // 子类继承父类，那么子类就可以当作一个父类来使用
    {
    public:
        PlaceHolder(const T& val)
        :_val(val)
        { }

        virtual const std::type_info& type()  // 获取子类对象保存类型
        {
            return typeid(T);
        }

        virtual Holder* clone() // 针对对象自身，克隆出一个新的子类对象
        {
            return new PlaceHolder(_val);
        }

    public:
        T _val;
    };
    Holder* _content;
};


// =======================================================
//                         测试
// =======================================================
void test_basic_type()
{
    Any a = 10;
    int* p = a.Get<int>();

    assert(*p == 10);
    std::cout << "test_basic_type passed\n";
}

void test_string_type()
{
    Any a = std::string("hello");
    std::string* p = a.Get<std::string>();

    assert(*p == "hello");
    std::cout << "test_string_type passed\n";
}

void test_copy_constructor()
{
    Any a = 42;
    Any b = a;   // 拷贝构造

    int* pa = a.Get<int>();
    int* pb = b.Get<int>();

    assert(*pa == 42);
    assert(*pb == 42);
    assert(pa != pb); // 必须是深拷贝

    std::cout << "test_copy_constructor passed\n";
}

void test_assignment_operator()
{
    Any a = 100;
    Any b;

    b = a;   // 拷贝赋值

    int* pa = a.Get<int>();
    int* pb = b.Get<int>();

    assert(*pa == 100);
    assert(*pb == 100);
    assert(pa != pb); // 深拷贝

    std::cout << "test_assignment_operator passed\n";
}

void test_reassign_different_type()
{
    Any a = 10;
    a = std::string("world");

    std::string* p = a.Get<std::string>();
    assert(*p == "world");

    std::cout << "test_reassign_different_type passed\n";
}

void test_polymorphism_behavior()
{
    Any a = 3.14;

    // 这里 Any 内部实际上是：
    // Holder* -> PlaceHolder<double>
    double* p = a.Get<double>();

    assert(*p == 3.14);
    std::cout << "test_polymorphism_behavior passed\n";
}

int main()
{
    test_basic_type();
    test_string_type();
    test_copy_constructor();
    test_assignment_operator();
    test_reassign_different_type();
    test_polymorphism_behavior();

    std::cout << "All tests passed!\n";
    return 0;
}