import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  build: {
    manifest: true,
    // Plotly is an intentionally lazy vendor chunk; keep its size warning out of the application build output.
    chunkSizeWarningLimit: 5000,
    rollupOptions: {
      input: {
        application: 'index.html',
      },
    },
  },
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.ts', 'src/**/*.test.tsx'],
    restoreMocks: true,
    setupFiles: ['./src/test/setup.ts'],
  },
});
