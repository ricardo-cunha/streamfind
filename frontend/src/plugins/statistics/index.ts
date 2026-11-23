import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { STATISTICS_RESULT_CONTRACTS } from './statisticsPlots';
import { StatisticsDashboard } from './StatisticsDashboard';

export const statisticsFrontendPlugin: FrontendPlugin = {
  manifest: {
    schemaVersion: 1,
    kind: 'viewer',
    id: 'native.viewer.statistics',
    name: 'streamfind statistics viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    plugins: ['statistics'],
    artifactContracts: STATISTICS_RESULT_CONTRACTS.map((contract) => `https://streamfind.dev/catalogue/plugins/statistics#${contract}`),
    capabilities: ['statistics-linked-results-dashboard'],
  },
  setup(api) {
    api.registerViewer({
      id: 'statistics.results-dashboard',
      label: 'Statistics results dashboard',
      accepts: STATISTICS_RESULT_CONTRACTS.map((contract) => `https://streamfind.dev/catalogue/plugins/statistics#${contract}`),
      component: StatisticsDashboard,
    });
  },
};

export default statisticsFrontendPlugin;
