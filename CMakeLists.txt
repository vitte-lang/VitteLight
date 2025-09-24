cmake_minimum_required(VERSION 3.16)

# Nom du projet
project(vittelight C)

# Toujours utiliser C11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)


# Inclure les répertoires
include_directories(
        .
        compiler
        core
        includes
        interpreter
        libraries
)

# Binaire principal
add_executable(VitteLight
        compiler/vitlc.c
        interpreter/vitli.c

        core/api.c
        core/code.c
        core/ctype.c
        core/debug.c
        core/do.c
        core/dump.c
        core/func.c
        core/gc.c
        core/lex.c
        core/mem.c
        core/object.c
        core/opcodes.c
        core/opnames.c
        core/parser.c
        core/state.c
        core/string.c
        core/table.c
        core/tm.c
        core/types.c
        core/undump.c
        core/utf8.c
        core/vl_compat.c
        core/vm.c
        core/zio.c

        libraries/amqp.c
        libraries/archive.c
        libraries/audio.c
        libraries/auxlib.c
        libraries/baselib.c
        libraries/cbor.c
        libraries/codec.c
        libraries/config.c
        libraries/corolib.c
        libraries/crypto.c
        libraries/csv.c
        libraries/curl.c
        libraries/dblib.c
        libraries/dl.c
        libraries/ffi.c
        libraries/ffmpeg.c
        libraries/font.c
        libraries/fs.c
        libraries/fswatch.c
        libraries/hash.c
        libraries/http.c
        libraries/img.c
        libraries/init.c
        libraries/iolib.c
        libraries/ioloop.c
        libraries/ipc.c
        libraries/json.c
        libraries/kafka.c
        libraries/kv.c
        libraries/libc.c
        libraries/libm.c
        libraries/loadlib.c
        libraries/log.c
        libraries/mathlib.c
        libraries/metrics.c
        libraries/mgpack.c
        libraries/mqtt.c
        libraries/mysql.c
        libraries/ncurses.c
        libraries/net.c
        libraries/oslib.c
        libraries/path.c
        libraries/pg.c
        libraries/proc.c
        libraries/protobuf.c
        libraries/pthread.c
        libraries/re.c
        libraries/redis.c
        libraries/rt.c
        libraries/sandbox.c
        libraries/strlib.c
        libraries/tablib.c
        libraries/threadpool.c
        libraries/time.c
        libraries/url.c
        libraries/utf8lib.c
        libraries/uuid.c
        libraries/webp.c
        libraries/ws.c
        libraries/xml.c
        libraries/yaml.c
        libraries/z.c
        libraries/zmq.c
)
