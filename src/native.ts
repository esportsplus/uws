import { execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { copyFileSync, existsSync, mkdirSync, readFileSync, readdirSync, statSync, unlinkSync } from 'node:fs';
import { availableParallelism, release } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';


type Dependency = {
    commit: string;
    url: string;
};

type Libraries = {
    crypto: string;
    ssl: string;
};

type NodeVersion = {
    abi: string;
    name: string;
};

type Toolchain = {
    cc: string;
    cxx: string;
};


const ARCH = ({ arm: 'arm', arm64: 'arm64', x64: 'x64' } as Record<string, string>)[process.arch];

const ASAN = process.argv.includes('--asan');

const DEPS: Record<string, Dependency> = {
    boringssl: {
        commit: 'c66572ad4dcdbd388a2802378a7e422a7f7d82e1',
        url: 'https://github.com/google/boringssl'
    }
};

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const DIRS = {
    addon: join(ROOT, 'native', 'addon'),
    build: join(ROOT, 'build'),
    deps: join(ROOT, 'deps'),
    dist: join(ROOT, 'dist'),
    usockets: join(ROOT, 'native', 'usockets'),
    uwebsockets: join(ROOT, 'native', 'uwebsockets')
};

/* One ABI per supported Node major. Add Node 27's ABI here the same way (its `process.versions.modules`
 * and a pinned `vX.Y.Z`) once it ships and the addon is validated against it on the box. */
const NODE_VERSIONS: NodeVersion[] = [
    { abi: '147', name: 'v26.8.1' }
];

const OS = ({ darwin: 'darwin', linux: 'linux', win32: 'win32' } as Record<string, string>)[process.platform];

const LIBC = OS !== 'linux' ? '' : ((process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined)?.header?.glibcVersionRuntime ? '' : '_musl');

const BORINGSSL_CACHE_KEY = createHash('sha256').update(JSON.stringify({
    source: readFileSync(fileURLToPath(import.meta.url), 'utf8').replace(/\r\n/g, '\n'),
    platform: [OS, LIBC, ARCH, release(), ASAN],
    environment: Object.fromEntries([
        'ImageOS', 'ImageVersion', 'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS',
        'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET', 'WindowsSDKVersion', 'VCToolsVersion',
        'CMAKE_GENERATOR', 'CMAKE_TOOLCHAIN_FILE'
    ].map((key) => [key, process.env[key] ?? ''])),
    tools: [OS === 'win32' ? 'clang' : (process.env.CC ?? 'cc'), OS === 'win32' ? 'clang++' : (process.env.CXX ?? 'c++'), 'cmake']
        .map((command) => execSync(`${command} --version`, { encoding: 'utf8' }).trim())
})).digest('hex');


function boringssl(arch: string): Libraries {
    let dir = join(DIRS.deps, 'boringssl'),
        out = join(dir, arch, BORINGSSL_CACHE_KEY),
        cache = join(DIRS.deps, 'boringssl-cache', BORINGSSL_CACHE_KEY, arch),
        names = OS === 'win32' ? { crypto: 'crypto.lib', ssl: 'ssl.lib' } : { crypto: 'libcrypto.a', ssl: 'libssl.a' },
        libs = { crypto: join(cache, names.crypto), ssl: join(cache, names.ssl) };

    if (existsSync(libs.crypto) && existsSync(libs.ssl)) {
        console.log(`--> Using cached BoringSSL libraries for ${arch}`);
        return libs;
    }

    if (OS === 'win32') {
        run(`cmake -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release -GNinja -B "${out}" "${dir}"`);
    }
    else {
        let osx = OS === 'darwin' ? ` -DCMAKE_OSX_ARCHITECTURES=${arch === 'x64' ? 'x86_64' : arch}` : '';

        run(`cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release${osx} -B "${out}" "${dir}"`);
    }

    run(`cmake --build "${out}" --parallel ${availableParallelism()} --target crypto ssl`);
    mkdirSync(cache, { recursive: true });
    copyFileSync(join(out, names.crypto), libs.crypto);
    copyFileSync(join(out, names.ssl), libs.ssl);

    return libs;
}

function compileAddon(arch: string, version: NodeVersion): void {
    let includes = [
            `-I"${DIRS.usockets}"`,
            `-I"${DIRS.uwebsockets}"`,
            `-I"${join(DIRS.deps, 'boringssl', 'include')}"`,
            `-I"${join(DIRS.deps, 'targets', `node-${version.name}`, 'include', 'node')}"`
        ],
        defines = ['-DLIBUS_USE_LIBUV', '-DLIBUS_USE_OPENSSL', '-DUWS_REMOTE_ADDRESS_USERSPACE', '-DUWS_WITH_PROXY', '-DWIN32_LEAN_AND_MEAN'];

    let flags = [...defines, ...includes, optimize(), '-c'].join(' ');

    if (OS !== 'win32') {
        flags += ' -fPIC -pthread -fvisibility=hidden';
    }

    run(`${toolchain(arch).cxx} ${flags} -std=c++20 "${join(DIRS.addon, 'addon.cpp')}"`, DIRS.build);
}

function compileObjects(arch: string): void {
    let includes = [
            `-I"${DIRS.usockets}"`,
            `-I"${DIRS.uwebsockets}"`,
            `-I"${join(DIRS.deps, 'boringssl', 'include')}"`,
            `-I"${join(DIRS.deps, 'targets', `node-${NODE_VERSIONS[0].name}`, 'include', 'node')}"`
        ],
        defines = ['-DLIBUS_USE_LIBUV', '-DLIBUS_USE_OPENSSL', '-DUWS_REMOTE_ADDRESS_USERSPACE', '-DUWS_WITH_PROXY', '-DWIN32_LEAN_AND_MEAN'],
        cSources = [
            ...sources(DIRS.usockets, '.c'),
            ...sources(join(DIRS.usockets, 'crypto'), '.c'),
            ...sources(join(DIRS.usockets, 'eventing'), '.c')
        ];

    let flags = [...defines, ...includes, optimize(), '-c'].join(' ');

    if (OS !== 'win32') {
        flags += ' -fPIC -pthread -fvisibility=hidden';
    }

    run(`${toolchain(arch).cc} ${flags} ${quote(cSources)}`, DIRS.build);
    run(`${toolchain(arch).cxx} ${flags} -std=c++20 "${join(DIRS.usockets, 'crypto', 'sni_tree.cpp')}"`, DIRS.build);
}

function fetch(): void {
    mkdirSync(DIRS.build, { recursive: true });
    mkdirSync(DIRS.deps, { recursive: true });
    mkdirSync(DIRS.dist, { recursive: true });
    mkdirSync(join(DIRS.deps, 'targets'), { recursive: true });

    for (let key of ['boringssl']) {
        let dep = DEPS[key],
            dir = join(DIRS.deps, key);

        if (!existsSync(dir)) {
            run(`git clone --recursive "${dep.url}" "${dir}"`);
        }

        run(`git -C "${dir}" checkout --recurse-submodules ${dep.commit}`);
    }

    for (let version of NODE_VERSIONS) {
        let target = join(DIRS.deps, 'targets', `node-${version.name}`);

        if (existsSync(target)) {
            continue;
        }

        run(`curl -fL "https://nodejs.org/dist/${version.name}/node-${version.name}-headers.tar.gz" -o node-${version.name}-headers.tar.gz`, DIRS.deps);
        run(`tar xzf node-${version.name}-headers.tar.gz -C targets`, DIRS.deps);

        if (OS === 'win32') {
            run(`curl -fL "https://nodejs.org/dist/${version.name}/win-x64/node.lib" -o "${join(target, 'node.lib')}"`);
        }

        run(`curl -fL "https://raw.githubusercontent.com/nodejs/node/${version.name}/deps/v8/include/v8-fast-api-calls.h" -o "${join(target, 'include', 'node', 'v8-fast-api-calls.h')}"`);
    }
}

function link(arch: string, version: NodeVersion, libs: Libraries): void {
    let cxx = toolchain(arch).cxx,
        objects = quote(sources(DIRS.build, '.o')),
        output = join(DIRS.dist, `uws_${OS}_${arch}${LIBC}_${version.abi}.node`);

    if (OS === 'win32') {
        run(`${cxx} -O3 ${objects} "${libs.ssl}" "${libs.crypto}" "${join(DIRS.deps, 'targets', `node-${version.name}`, 'node.lib')}" -ladvapi32 -std=c++20 -shared -o "${output}"`);
    }
    else if (OS === 'darwin') {
        run(`${cxx} -pthread ${optimize()} ${objects} "${libs.ssl}" "${libs.crypto}" -std=c++20 -shared -undefined dynamic_lookup${ASAN ? ' -fsanitize=address' : ''} -o "${output}"`);
    }
    else {
        let linker = process.env.UWS_LD ? ` -fuse-ld=${process.env.UWS_LD}` : '';

        run(`${cxx} -pthread ${optimize()}${linker} ${objects} "${libs.ssl}" "${libs.crypto}" -std=c++20 -shared ${ASAN ? '-fsanitize=address' : '-static-libstdc++ -static-libgcc -s'} -o "${output}"`);
    }
}

function optimize(): string {
    if (ASAN) {
        return '-fsanitize=address -fno-omit-frame-pointer -g -O1';
    }

    return OS === 'win32' ? '-O3' : '-flto -O3';
}

function quote(paths: string[]): string {
    return paths.map((p) => `"${p}"`).join(' ');
}

function run(command: string, cwd: string = ROOT): void {
    console.log(`--> ${command}\n`);
    execSync(command, { cwd, stdio: 'inherit' });
}

function sources(dir: string, extension: string): string[] {
    return readdirSync(dir).filter((f) => f.endsWith(extension)).map((f) => join(dir, f));
}

function toolchain(arch: string): Toolchain {
    let cc = process.env.UWS_CC ?? (OS === 'linux' ? 'clang-18' : 'clang'),
        cxx = process.env.UWS_CXX ?? (OS === 'linux' ? 'clang++-18' : 'clang++');

    if (OS === 'win32') {
        return { cc: `${cc} -fms-runtime-lib=static`, cxx: `${cxx} -fms-runtime-lib=static` };
    }
    else if (OS === 'darwin') {
        let target = `-target ${arch === 'x64' ? 'x86_64' : arch}-apple-macos12`;

        return { cc: `${cc} ${target}`, cxx: `${cxx} -stdlib=libc++ ${target}` };
    }

    return { cc, cxx };
}

function addonCacheKey(): string {
    let hash = createHash('sha256').update(BORINGSSL_CACHE_KEY);

    function addDirectory(dir: string): void {
        for (let entry of readdirSync(dir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
            let path = join(dir, entry.name);

            hash.update(entry.name).update('\0');

            if (entry.isDirectory()) {
                addDirectory(path);
            }
            else {
                hash.update(readFileSync(path)).update('\0');
            }
        }
    }

    addDirectory(join(ROOT, 'native'));
    hash.update(JSON.stringify({
        abi: process.versions.modules,
        toolchains: (OS === 'darwin' ? ['x64', 'arm64'] : [ARCH]).map(toolchain),
        linker: process.env.UWS_LD ?? '',
        tools: [process.env.UWS_CC ?? (OS === 'linux' ? 'clang-18' : 'clang'), process.env.UWS_CXX ?? (OS === 'linux' ? 'clang++-18' : 'clang++'), ...(process.env.UWS_LD ? [`ld.${process.env.UWS_LD}`] : [])]
            .map((command) => execSync(`${command} --version`, { encoding: 'utf8' }).trim())
    }));

    return hash.digest('hex');
}


if (process.argv.includes('--boringssl-cache-key')) {
    console.log(BORINGSSL_CACHE_KEY);
    process.exit(0);
}

if (process.argv.includes('--addon-cache-key')) {
    console.log(addonCacheKey());
    process.exit(0);
}

if (process.argv.includes('--check-native')) {
    for (let arch of OS === 'darwin' ? ['x64', 'arm64'] : [ARCH]) {
        for (let version of NODE_VERSIONS) {
            let path = join(DIRS.dist, `uws_${OS}_${arch}${LIBC}_${version.abi}.node`);

            if (!existsSync(path) || !statSync(path).size) {
                throw new Error(`[uws/native] Missing or empty native binary: ${path}`);
            }
        }
    }

    process.exit(0);
}

fetch();

for (let arch of OS === 'darwin' ? ['x64', 'arm64'] : [ARCH]) {
    let libs = boringssl(arch);

    compileObjects(arch);

    for (let version of NODE_VERSIONS) {
        compileAddon(arch, version);
        link(arch, version, libs);
    }

    for (let object of sources(DIRS.build, '.o')) {
        unlinkSync(object);
    }
}
