import { defineConfig } from 'vitest/config';


export default defineConfig({
    test: {
        environment: 'node',
        globals: true,
        globalSetup: ['./test/setup/certs.ts'],
        hookTimeout: 20000,
        include: ['test/**/*.test.ts'],
        maxWorkers: process.env.CI ? 2 : undefined,
        passWithNoTests: true,
        pool: 'forks',
        testTimeout: 10000
    }
});
