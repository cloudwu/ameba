#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int g_chan_id = 0;

struct alloc_ud {
	size_t mem;
	size_t peak;
};

static void * 
_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	struct alloc_ud * au = (struct alloc_ud *) ud;
	if (nsize == 0) {
		au->mem -= osize;
		free(ptr);
		return NULL;
	}
    else {
		au->mem += nsize - osize;
		if (au->mem > au->peak) {
			au->peak = au->mem;
		}
		return realloc(ptr, nsize);
	}
}

enum thread_stat {
	THREAD_RUNNING = 1,
	THREAD_SUSPEND = 2,
	THREAD_BLOCKED = 3,
};

struct thread {
	struct thread * next;
	lua_State *L;
	enum thread_stat stat;
	int chan;
	struct alloc_ud mem;
};

static void
_hook(lua_State *L, lua_Debug *ar)
{
	lua_yield(L,0);
}

#define QUEUE_READ 1
#define QUEUE_WRITE 2

static int
_recv(lua_State *L) {
	lua_settop(L,0);
	lua_getfield(L,LUA_REGISTRYINDEX,"ameba.chan");	// L 1 ameba.chan
	size_t s = lua_rawlen(L,1);
	if (s>=1) {
		size_t i;
		for (i=1;i<s;i++) {
			lua_rawgeti(L,1,i+1);
		}
		lua_rawgeti(L,1,1);
		for (i=1;i<=s;i++) {
			lua_pushnil(L);
			lua_rawseti(L,1,i);
		}
		lua_replace(L,1);
		return s;
	}

	lua_State *mL = lua_touserdata(L,lua_upvalueindex(1));
	lua_checkstack(mL , 5);

	lua_getfield(mL, LUA_REGISTRYINDEX,"ameba"); // mL -1 ameba
	struct thread * current = lua_touserdata(L,lua_upvalueindex(2));

	lua_rawgeti(mL,-1,current->chan); // mL -1 chan table , -2 ameba
	if (lua_isnil(mL,-1)) {
		lua_pop(mL,2);
		lua_pushboolean(L,0);
		return 1;
	}

	lua_rawgeti(mL,-1,QUEUE_READ); // mL -1 read queue, -2 chan table , -3 ameba
	s = lua_rawlen(mL,-1);
	lua_pushlightuserdata(mL,current);
	lua_rawseti(mL,-2,s+1);
	current->stat = THREAD_BLOCKED;

	lua_rawgeti(mL,-2,QUEUE_WRITE); // mL -1 write queue , -2 read queue, -3 chan table , -4 ameba
	s = lua_rawlen(mL,-1);
	if (s>0) {
		// writer in queue
		lua_rawgeti(mL , -1 , s);	// mL -1 writer , -2 write queue , -3 read queue, -4 chan table , -5 ameba
		struct thread * writer = lua_touserdata(mL,-1);
		if (writer->stat == THREAD_SUSPEND) {
			writer->next = current->next;
			current->next = writer;
		}
		writer->stat = THREAD_RUNNING;

		lua_pushnil(mL);
		lua_rawseti(mL,-3,s);
		lua_pop(mL,5);
	}

	lua_yield(L,0);
	return 0;
}

static int 
_send(lua_State *L) {
	lua_State *mL = lua_touserdata(L,lua_upvalueindex(1));
	lua_checkstack(mL , 5);

	lua_getfield(mL, LUA_REGISTRYINDEX,"ameba"); // mL -1 ameba

	int idx = luaL_checkinteger(L,1);
	lua_rawgeti(mL,-1,idx);	// mL -1 chan table , -2 ameba
	if (lua_isnil(mL,-1)) {
		lua_pop(mL,2);
		lua_pushboolean(L,0);
		return 1;
	}

	struct thread * current = lua_touserdata(L,lua_upvalueindex(2));

	lua_rawgeti(mL , -1 , QUEUE_READ);	// mL -1 read queue , -2 chan table , -3 ameba

	size_t s = lua_rawlen(mL,-1);
	if (s > 0) {
		// reader suspend in queue
		lua_rawgeti(mL , -1 , s);	// mL -1 reader , -2 read queue , -3 chan table , -4 ameba
		struct thread * reader = lua_touserdata(mL,-1);
		lua_getfield(reader->L,LUA_REGISTRYINDEX,"ameba.chan");	// readerL -1 ameba.chan
		int i;
		int top = lua_gettop(L);
		for (i=2;i<=top;i++) {
			int t = lua_type(L,i);
			size_t size;
			const char * str;
			switch (t) {
			case LUA_TNUMBER:
				lua_pushnumber(reader->L, lua_tonumber(L,i));
				break;
			case LUA_TBOOLEAN:
				lua_pushboolean(reader->L, lua_toboolean(L,i));
				break;
			case LUA_TSTRING:
				str = lua_tolstring(L,i,&size);
				lua_pushlstring(reader->L , str, size);
				break;
			default:
				return luaL_error(L,"%s unsupported.",t);
			}
			lua_rawseti(reader->L,-2,i-1);
		}
		lua_pop(reader->L,1);
		lua_pushnil(mL);	// mL -1 nil , -2 reader , -3 read queue , -4 chan table , -5 ameba
		lua_rawseti(mL,-3,s); // mL -1 reader , -2 read queue , -3 chan table , -4 ameba
		lua_pop(mL,4);
		
		if (reader->stat == THREAD_SUSPEND) {
			reader->next = current->next;
			current->next = reader;
		}
		reader->stat = THREAD_RUNNING;

		lua_pushboolean(L,1);

		return 1;
	}

	lua_rawgeti(mL , -2 , QUEUE_WRITE);	// mL -1 write queue, -2 read queue , -3 chan table , -4 ameba
	s = lua_rawlen(mL,-1);
	lua_pushlightuserdata(mL, current);	// mL -1 current thread , -2 write queue, -3 read queue , -4 chan table , -5 ameba
	lua_rawseti(mL, -2 , s + 1);	// mL -1 write queue, -2 read queue , -3 chan table , -4 ameba
	current->stat = THREAD_BLOCKED;
	lua_pop(mL,4);

	lua_yield(L,0);
	return 0;
}

static int _ameba(lua_State *L);

static const char * _wrap =
"local _recv = __recv" "\n"
"local _send = __send" "\n"

"__recv , __send = nil,nil" "\n"

"local function __recv(ret,...)" "\n"
"	if ret == nil then" "\n"
"		return recv()" "\n"
"	elseif ret == false then" "\n"
"		return" "\n"
"	else" "\n"
"		return ret , ..." "\n"
"	end" "\n"
"end" "\n"

"function recv()" "\n"
"	return __recv(_recv())" "\n"
"end" "\n"

"function send(...)" "\n"
"	repeat" "\n"
"		local ret = _send(...)" "\n"
"		if ret == false then" "\n"
"			return" "\n"
"		end" "\n"
"	until ret == true" "\n"
"end" "\n"
;

static const luaL_Reg _funcs[] = {
	{"ameba", _ameba},
	{"__send", _send},
	{"__recv", _recv},
	{NULL, NULL}
};

// lightuserdata thread
// string src
static int
_new_ameba(lua_State *L) {
	struct thread * head = lua_touserdata(L,1);
	struct thread * node = malloc(sizeof(*node));
	node->stat = THREAD_RUNNING;
	node->next = head->next;
	memset(&node->mem,0,sizeof(node->mem));
	node->L = lua_newstate(_alloc, &node->mem);
	luaopen_base(node->L);
	lua_getglobal(node->L, "load");
	size_t size = 0;
	const char * src = luaL_checklstring(L,2,&size);
	lua_pushlstring(node->L,src,size);
	lua_call(node->L, 1 , 2 );
	if (lua_isnil(node->L, -2)) {
		const char * err = luaL_checklstring(node->L , -1 , &size);
		fprintf(stderr,"%s\n",err);
//		lua_pushlstring(L, err , size);
		lua_close(node->L);
		free(node);
		return 0;
	}
	node->chan = ++ g_chan_id;
	lua_pop(node->L,1);
	head->next = node;

	lua_newtable(node->L);
	lua_setfield(node->L,LUA_REGISTRYINDEX,"ameba.chan");

	lua_pushglobaltable(node->L);
	lua_pushlightuserdata(node->L , L);
	lua_pushlightuserdata(node->L , node);

	luaL_setfuncs(node->L, _funcs , 2);

	luaL_loadstring(node->L, _wrap);
	lua_call(node->L, 0, 0);

	lua_pushinteger(node->L , g_chan_id);
	lua_setfield(node->L, -2 , "chan");
	lua_pop(node->L,1);
	lua_gc(node->L,LUA_GCCOLLECT,0);

	lua_sethook(node->L,_hook,LUA_MASKCOUNT,64);


	lua_getfield(L,LUA_REGISTRYINDEX,"ameba");
	lua_createtable(L,2,0);
	lua_newtable(L);
	lua_rawseti(L,-2,QUEUE_READ);
	lua_newtable(L);
	lua_rawseti(L,-2,QUEUE_WRITE);
	lua_rawseti(L,-2,g_chan_id);

	lua_pushinteger(L , g_chan_id);

	return 1;	
}

static int
_ameba(lua_State *L) {
	struct lua_State *mL = lua_touserdata(L,lua_upvalueindex(1));
	struct thread * head = lua_touserdata(L,lua_upvalueindex(2));
	lua_pushcfunction(mL,_new_ameba);
	lua_pushlightuserdata(mL, head);
	size_t size = 0;
	const char * src = luaL_checklstring(L, 1 , &size);
	lua_pushlstring(mL,src,size);
	lua_call(mL,2,1);
	if (lua_isnil(mL,-1)) {
		return 0;
	}
	int chan = lua_tointeger(mL,-1);
	lua_pushinteger(L,chan);
	return 1;
}

static void
_wakeup(lua_State *L, int chan, struct thread *head) {
	lua_rawgeti(L,1,chan);
	int i;
	for (i=QUEUE_READ;i<=QUEUE_WRITE;i++) {
		lua_rawgeti(L,-1,i);
		int s = lua_rawlen(L,-1);
		int j;
		for (j=1;j<=s;j++) {
			lua_rawgeti(L,-1,j);
			struct thread * thr = lua_touserdata(L,-1);
			if (thr->stat == THREAD_SUSPEND) {
				thr->next = head->next;
				head->next = thr;
			}
			thr->stat = THREAD_RUNNING;
			lua_pop(L,1);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

static int
ameba_run(lua_State *L) {
	struct thread *head = calloc(1,sizeof(*head));
	head->next = head;
	lua_pushcfunction(L,_new_ameba);
	lua_pushlightuserdata(L,head);
	lua_pushvalue(L,1);
	lua_call(L,2,1);
	if (lua_isnil(L,-1)) {
		return 0;
	}
	lua_settop(L,0);
	lua_getfield(L, LUA_REGISTRYINDEX,"ameba"); 

	for (;;) {
		if (head->next == head)
			return 0;
		lua_State *nL = head->next->L;
		if (nL != NULL) {
			if (head->next->stat == THREAD_BLOCKED) {
				head->next->stat = THREAD_SUSPEND;
				head->next = head->next->next;
				continue;
			}
			int r = lua_resume(nL , 0);
			if (r == LUA_YIELD) {
				head = head->next;
				continue;
			}
			struct thread * current = head->next;
			head->next = current->next;
			if (r != LUA_OK) {
				const char * err = lua_tostring(nL,-1);
				fprintf(stderr,"%s\n",err);
			}
			lua_close(nL);
			_wakeup(L,current->chan,head);
			lua_pushnil(L);
			lua_rawseti(L,1,current->chan);
//			printf("close %d %d\n",current->chan,current->mem.peak);
			free(current);
		} else {
			head = head->next;
		}
	}
}

static const luaL_Reg ameba_funcs[] = {
	{"run", ameba_run},
	{NULL, NULL}
};

LUAMOD_API int 
luaopen_ameba (lua_State *L) {
	luaL_newlib(L,ameba_funcs);
	lua_newtable(L);
	lua_setfield(L,LUA_REGISTRYINDEX,"ameba");
	return 1;
}
