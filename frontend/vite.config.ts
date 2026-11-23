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
      output: {
        manualChunks(id) {
          const normalized = id.replaceAll('\\', '/');
          if (normalized.includes('/node_modules/react/') || normalized.includes('/node_modules/react-dom/')) {
            return 'react-runtime';
          }
          if (normalized.includes('/src/framework/')) return 'streamfind-framework';
          return undefined;
        },
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
