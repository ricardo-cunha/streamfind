import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { STATISTICS_ARTIFACT_CONTRACTS } from './statisticsPlots';
import { StatisticsDashboard } from './StatisticsDashboard';

export const statisticsFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.statistics',
    name: 'streamfind statistics viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    domains: ['statistics'],
    artifactContracts: [...STATISTICS_ARTIFACT_CONTRACTS],
    capabilities: ['statistics-linked-results-dashboard'],
  },
  setup(api) {
    api.registerViewer({
      id: 'statistics.results-dashboard',
      label: 'Statistics results dashboard',
      accepts: [...STATISTICS_ARTIFACT_CONTRACTS],
      component: StatisticsDashboard,
    });
  },
};

export default statisticsFrontendPlugin;
