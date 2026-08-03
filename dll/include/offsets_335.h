// 3.3.5.12340 Lua 接口地址 —— 证据记录
//
// 本文件只做证据留存,不参与编译。地址的唯一来源是 src/lua_interface.cpp,
// 这里记录每个地址是「怎么被确定下来的」,以便日后换客户端时能重做一遍验证。
//
// 目标 exe : D:\ayu_space\game\sfwow\80\circle 3.3.5.12340 zhCN\Wow.exe
//   大小   : 7,704,216 字节
//   MD5    : 45892bdedd0ad70aed4ccd22d9fb5984
//   SHA1   : 178f78380affd260cb775d44397ba6b33ac05fdb
//   PE 链接时间戳 : 2010-06-25(原版 Blizzard 12340;文件系统的 2019 日期只是复制时间)
//   ImageBase 0x400000    .text 0x401000 - 0x9DE3B3
//   私服补丁是以追加节 .zdata(0xDD1000, RWX, 1 页)的方式做的,原版代码段未重排,
//   所以原版的函数布局仍然成立。
//
// ── 从 1.12 移植过来时必须改的三处(已全部处理)────────────────────────────
//
// 1. 调用约定是 __cdecl,不是 __fastcall。
//    实测 UnitXP(0x60EA60) 开头:
//        8b 75 08              mov esi,[ebp+8]      ; L 从栈取,不是 ecx
//        6a 01 56              push 1; push esi     ; 参数右→左入栈
//        e8 ....               call 0x84DF60
//        83 c4 08              add esp,8            ; 调用方清栈
//    以及 pushnumber 处 `add esp,0xC` = 4(L) + 8(double),完全对得上 cdecl。
//
// 2. lua_tostring 在 3.3.5 是 3 参数的 lua_tolstring(L, idx, size_t* len);
//    lua_tostring 本身只是个宏。实测 UnitXP 处:
//        6a 00 6a 01 56        push 0; push 1; push esi   ; 第 3 参传了 NULL
//        e8 ....               call 0x84E0E0
//
// 3. 1.12 代码里的 p_GetContext(0x7040D0)/GetLuaContext() 是死代码,
//    全项目 grep 确认从未被调用,已删除。L 由 hook 的栈参数直接给出,不需要它。
//
// ── 地址表 ─────────────────────────────────────────────────────────────────
//
//   函数              1.12 原值    3.3.5 实测   判据(反汇编所见)
//   lua_gettop        0x6F3070     0x84DBD0     mov eax,[ecx+0xC]; sub eax,[ecx+0x10];
//                                               sar eax,4  = (top-base)/16
//   lua_isnumber      0x6F34D0     0x84DF20     cmp [eax+8],3;否则 call luaV_tonumber
//   lua_isstring      0x6F3510     0x84DF60     tt==4(STRING) || tt==3(NUMBER)
//   lua_tonumber      0x6F3620     0x84E030     同 isnumber,但 fld/fldz 返回 double
//   lua_toboolean     0x6F3660     0x84E0B0     tt==0→false; tt==1→value.b; 其余→true
//   lua_tolstring     0x6F3690     0x84E0E0     3 参数(见上文第 2 点)
//   lua_pushnil       0x6F37F0     0x84E280     mov [ecx+8],0 (tt=NIL); add [eax+0xC],0x10
//   lua_pushnumber    0x6F3810     0x84E2A0     fld qword[ebp+0xC]; mov [eax+8],3 (tt=NUMBER)
//   lua_pushstring    0x6F3890     0x84E350     s==NULL→内联 pushnil,否则内联 strlen
//                                               再调 pushlstring(0x84E300)
//   lua_pushboolean   0x6F39F0     0x84E4D0     setne dl; mov [eax+8],1 (tt=BOOLEAN)
//
//   UnitXP(hook 目标) 0x517350     0x60EA60     由 FrameScript 注册表锚定:
//                                               表项 @0xAD22E8 -> name "UnitXP" @0xA1F430,
//                                               紧邻的下一个 DWORD 即函数地址
//
// ── 整体交叉验证 ───────────────────────────────────────────────────────────
//
// 上面 10 个 lua_* 的地址是严格递增的:
//   gettop < isnumber < isstring < tonumber < toboolean < tolstring
//         < pushnil < pushnumber < pushstring < pushboolean
// 这个顺序同时满足两件事:
//   (a) 与 Lua 5.1 lapi.c 里这些函数的源码定义顺序一致;
//   (b) 与 1.12 那 10 个地址的相对顺序完全同构
//       (0x6F3070 < 0x6F34D0 < 0x6F3510 < 0x6F3620 < 0x6F3660
//        < 0x6F3690 < 0x6F37F0 < 0x6F3810 < 0x6F3890 < 0x6F39F0)
// 所以这 10 个地址是一一对应的,没有错位。
//
// 另外,lua_pushstring 在 s==NULL 分支内联出来的 pushnil 代码,与独立的
// lua_pushnil(0x84E280) 逐字节相同 —— 又一道闭环。
//
// ── 顺带测出的运行时布局(如需扩展功能可用)─────────────────────────────
//
//   lua_State : top 在 +0xC,base 在 +0x10
//   TValue    : sizeof = 16,value 在 +0,tt 在 +8
//   tt 常量   : 0=NIL 1=BOOLEAN 3=NUMBER 4=STRING
//   全局 nilobject @ 0xA46F78    index2adr @ 0x84D9C0
//
//   同表锚定到的其它 Lua API(可用于二次校验):
//     UnitName        表项 0xAD22D8 -> 0x60E740
//     UnitXPMax       表项 0xAD22F0 -> 0x60EAE0
//     UnitIsDead      表项 0xAD2340 -> 0x60F480
//     GetTime         表项 0xAD2184 -> 0x6081F0
//     SendChatMessage 表项 0xAC7A58 -> 0x50D170
//     GetItemInfo     表项 0xAC8728 -> 0x516C60
//
// 复现这些结论的脚本在 tools/ 下(pe.js / find_api.js / dump.js / lua_scan.js /
// find_push.js),用 Node 跑,不需要装任何编译器或反汇编器。
