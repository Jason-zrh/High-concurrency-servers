#pragma once
#include "Common.h"

// 定长内存池
//template<size_t N>
//class ObjectPool
//{};

template<class T>
class ObjectPool
{
public:
	T* New()
	{
		T* obj = nullptr;

		// 优先把还回来内存块对象，再次重复利用
		if (_freeList)
		{
			void* next = *((void**)_freeList);
			obj = (T*)_freeList;
			_freeList = next;
		}
		else
		{
			// 剩余内存不够一个对象大小时，则重新开大块空间
			if (_remainBytes < sizeof(T))
			{
				_remainBytes = 128 * 1024;
				//_memory = (char*)malloc(_remainBytes);
				_memory = (char*)SystemAlloc(_remainBytes >> 13);
				if (_memory == nullptr)
				{
					throw std::bad_alloc();
				}
			}

			obj = (T*)_memory;
			size_t objSize = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
			_memory += objSize;
			_remainBytes -= objSize;
		}

		// 定位new，显示调用T的构造函数初始化
		new(obj)T;

		return obj;
	}

	void Delete(T* obj)
	{
		// 显示调用析构函数清理对象
		obj->~T();

		// 头插
		*(void**)obj = _freeList;
		_freeList = obj;
	}

private:
	char* _memory = nullptr; // 指向大块内存的指针
	size_t _remainBytes = 0; // 大块内存在切分过程中剩余字节数
	void* _freeList = nullptr; // 还回来过程中链接的自由链表的头指针
};