import { createRequire } from 'node:module';
import { getCACertificates } from 'node:tls';
import { MAX_U8, MAX_U16, PACKAGE_NAME } from './constants';
import type { ClientOptions, NativeModule, RecognizedString, TemplatedClient } from './types';


const load = createRequire(import.meta.url);

const textEncoder = new TextEncoder();


function libc(): string {
    if (process.platform !== 'linux') {
        return '';
    }

    return (process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined)?.header?.glibcVersionRuntime ? '' : '_musl';
}

function native(): NativeModule {
    try {
        return load(`../dist/uws_${process.platform}_${process.arch}${libc()}_${process.versions.modules}.node`);
    }
    catch (e) {
        throw new Error(`${PACKAGE_NAME}: no prebuilt binary for ${process.platform} ${process.arch} ABI ${process.versions.modules}. Supported: Node 26+ on glibc/musl Linux, macOS and Windows x64.`, { cause: e });
    }
}

function toUint8Array(value?: RecognizedString): Uint8Array {
    if (value === undefined) {
        return new Uint8Array(0);
    }

    if (typeof value === 'string') {
        return textEncoder.encode(value);
    }

    if (value instanceof ArrayBuffer || value instanceof SharedArrayBuffer) {
        return new Uint8Array(value);
    }

    return new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
}

function hasHeaderControlCharacters(bytes: Uint8Array, allowTab = false): boolean {
    for (let i = 0; i < bytes.length; i++) {
        let byte = bytes[i];

        if ((byte < 0x20 && (!allowTab || byte !== 0x09)) || byte === 0x7f) {
            return true;
        }
    }

    return false;
}

function validateHeader(key: RecognizedString, value: RecognizedString): void {
    let keyBytes = toUint8Array(key),
        valueBytes = toUint8Array(value);

    if (keyBytes.includes(0x3a) || hasHeaderControlCharacters(keyBytes) || hasHeaderControlCharacters(valueBytes, true)) {
        throw new Error('uWS: header contains control characters');
    }
}

function validateStatus(status: RecognizedString): void {
    if (hasHeaderControlCharacters(toUint8Array(status))) {
        throw new Error('uWS: header contains control characters');
    }
}


const uws = native();

const App = uws.App;

const Client = uws.Client;

const DEDICATED_COMPRESSOR = uws.DEDICATED_COMPRESSOR;

const DEDICATED_COMPRESSOR_3KB = uws.DEDICATED_COMPRESSOR_3KB;

const DEDICATED_COMPRESSOR_4KB = uws.DEDICATED_COMPRESSOR_4KB;

const DEDICATED_COMPRESSOR_8KB = uws.DEDICATED_COMPRESSOR_8KB;

const DEDICATED_COMPRESSOR_16KB = uws.DEDICATED_COMPRESSOR_16KB;

const DEDICATED_COMPRESSOR_32KB = uws.DEDICATED_COMPRESSOR_32KB;

const DEDICATED_COMPRESSOR_64KB = uws.DEDICATED_COMPRESSOR_64KB;

const DEDICATED_COMPRESSOR_128KB = uws.DEDICATED_COMPRESSOR_128KB;

const DEDICATED_COMPRESSOR_256KB = uws.DEDICATED_COMPRESSOR_256KB;

const DEDICATED_DECOMPRESSOR = uws.DEDICATED_DECOMPRESSOR;

const DEDICATED_DECOMPRESSOR_512B = uws.DEDICATED_DECOMPRESSOR_512B;

const DEDICATED_DECOMPRESSOR_1KB = uws.DEDICATED_DECOMPRESSOR_1KB;

const DEDICATED_DECOMPRESSOR_2KB = uws.DEDICATED_DECOMPRESSOR_2KB;

const DEDICATED_DECOMPRESSOR_4KB = uws.DEDICATED_DECOMPRESSOR_4KB;

const DEDICATED_DECOMPRESSOR_8KB = uws.DEDICATED_DECOMPRESSOR_8KB;

const DEDICATED_DECOMPRESSOR_16KB = uws.DEDICATED_DECOMPRESSOR_16KB;

const DEDICATED_DECOMPRESSOR_32KB = uws.DEDICATED_DECOMPRESSOR_32KB;

const DISABLED = uws.DISABLED;

const getParts = uws.getParts;

const LIBUS_LISTEN_EXCLUSIVE_PORT = uws.LIBUS_LISTEN_EXCLUSIVE_PORT;

const SHARED_COMPRESSOR = uws.SHARED_COMPRESSOR;

const SHARED_DECOMPRESSOR = uws.SHARED_DECOMPRESSOR;

const SSLApp = uws.SSLApp;

/** wss:// client. Defaults ca_pem to Node's default CA set when no CA option is given, so the
 * native layer verifies against it without the caller wiring up a bundle. */
const SSLClient = (options: ClientOptions = {}): TemplatedClient => {
    if (options.ca_file_name === undefined && options.ca_pem === undefined) {
        return uws.SSLClient({ ...options, ca_pem: getCACertificates('default').join('\n') });
    }

    return uws.SSLClient(options);
};

const us_listen_socket_close = uws.us_listen_socket_close;

const us_socket_local_port = uws.us_socket_local_port;


class DeclarativeResponse {
    instructions: number[] = [];

    private appendInstruction(opcode: number, ...values: RecognizedString[]): void {
        this.instructions.push(opcode);

        for (let i = 0, n = values.length; i < n; i++) {
            let bytes = toUint8Array(values[i]);

            if (bytes.byteLength > MAX_U8) {
                throw new RangeError(`${PACKAGE_NAME}: data length exceeds ${MAX_U8}`);
            }

            this.instructions.push(bytes.byteLength, ...bytes);
        }
    }

    private appendInstructionWithLength(opcode: number, value?: RecognizedString): void {
        let bytes = toUint8Array(value);

        if (bytes.byteLength > MAX_U16) {
            throw new RangeError(`${PACKAGE_NAME}: data length exceeds ${MAX_U16}`);
        }

        this.instructions.push(opcode, bytes.byteLength & 0xff, (bytes.byteLength >> 8) & 0xff, ...bytes);
    }

    end(value?: RecognizedString): ArrayBuffer {
        this.appendInstructionWithLength(0, value);

        return new Uint8Array(this.instructions).buffer;
    }

    write(value: RecognizedString): this {
        this.appendInstructionWithLength(5, value);

        return this;
    }

    writeBody(): this {
        this.instructions.push(2);

        return this;
    }

    writeHeader(key: RecognizedString, value: RecognizedString): this {
        validateHeader(key, value);
        this.appendInstruction(1, key, value);

        return this;
    }

    writeHeaderValue(key: RecognizedString): this {
        this.appendInstruction(4, key);

        return this;
    }

    writeParameterValue(key: RecognizedString): this {
        this.appendInstruction(6, key);

        return this;
    }

    writeQueryValue(key: RecognizedString): this {
        this.appendInstruction(3, key);

        return this;
    }

    writeStatus(status: RecognizedString): this {
        validateStatus(status);
        this.appendInstruction(7, status);

        return this;
    }
}


export {
    App,
    Client,
    DeclarativeResponse,
    DEDICATED_COMPRESSOR,
    DEDICATED_COMPRESSOR_3KB,
    DEDICATED_COMPRESSOR_4KB,
    DEDICATED_COMPRESSOR_8KB,
    DEDICATED_COMPRESSOR_16KB,
    DEDICATED_COMPRESSOR_32KB,
    DEDICATED_COMPRESSOR_64KB,
    DEDICATED_COMPRESSOR_128KB,
    DEDICATED_COMPRESSOR_256KB,
    DEDICATED_DECOMPRESSOR,
    DEDICATED_DECOMPRESSOR_512B,
    DEDICATED_DECOMPRESSOR_1KB,
    DEDICATED_DECOMPRESSOR_2KB,
    DEDICATED_DECOMPRESSOR_4KB,
    DEDICATED_DECOMPRESSOR_8KB,
    DEDICATED_DECOMPRESSOR_16KB,
    DEDICATED_DECOMPRESSOR_32KB,
    DISABLED,
    getParts,
    LIBUS_LISTEN_EXCLUSIVE_PORT,
    SHARED_COMPRESSOR,
    SHARED_DECOMPRESSOR,
    SSLApp,
    SSLClient,
    us_listen_socket_close,
    us_socket_local_port
};

export type {
    AppOptions,
    ClientOptions,
    CompressOptions,
    ConnectBehavior,
    ConnectError,
    HttpRequest,
    HttpResponse,
    ListenOptions,
    MultipartField,
    RecognizedString,
    TemplatedApp,
    TemplatedClient,
    us_listen_socket,
    us_socket,
    us_socket_context_t,
    WebSocket,
    WebSocketBehavior
} from './types';
