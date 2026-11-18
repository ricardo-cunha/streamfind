import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { OpcuaExplorerViewer } from './OpcuaExplorerViewer';
import './OpcuaExplorerViewer.css';

export const sensorsFrontendPlugin: FrontendPlugin = {
  manifest: {
    id: 'streamfind.sensors',
    name: 'streamfind OPC UA viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    domains: ['sensors'],
    artifactContracts: ['opcuaExploration'],
    capabilities: ['opcua-node-explorer'],
  },
  setup(api) {
    api.registerViewer({
      id: 'sensors.opcua-explorer',
      label: 'OPC UA network explorer',
      accepts: ['opcuaExploration'],
      component: OpcuaExplorerViewer,
    });
  },
};

export default sensorsFrontendPlugin;
