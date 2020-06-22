#pragma once

// This is taken (and slightly modified) from Common Oblivion Engine Framework:
// https://github.com/JRoush/Common-Oblivion-Engine-Framework/blob/master/Utilities/Memaddr.inl

template <typename TReturn, typename Tthis, typename T1>
__forceinline TReturn thisCall(Tthis _this, UInt32 _addr, T1 arg1)
{
	if (!_addr) return TReturn(0);
	class T {}; union { UInt32 x; TReturn(T::*m)(T1); } u = { _addr };
	return ((T*)_this->*u.m)(arg1);
}

template <typename TReturn, typename Tthis, typename T1, typename T2>
__forceinline TReturn thisCall(Tthis _this, UInt32 _addr, T1 arg1, T2 arg2)
{
	if (!_addr) return TReturn(0);
	class T {}; union { UInt32 x; TReturn(T::*m)(T1, T2); } u = { _addr };
	return ((T*)_this->*u.m)(arg1, arg2);
}