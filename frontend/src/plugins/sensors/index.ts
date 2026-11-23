import { FRONTEND_PLUGIN_API_VERSION, type FrontendPlugin } from '../../framework/plugins/pluginTypes';
import { OpcuaExplorerViewer } from './OpcuaExplorerViewer';
import './OpcuaExplorerViewer.css';

export const sensorsFrontendPlugin: FrontendPlugin = {
  manifest: {
    schemaVersion: 1,
    kind: 'viewer',
    id: 'native.viewer.sensors',
    name: 'streamfind OPC UA viewers',
    version: '1.0.0',
    apiVersion: FRONTEND_PLUGIN_API_VERSION,
    plugins: ['sensors'],
    artifactContracts: ['https://streamfind.dev/catalogue/plugins/sensors#opcuaExploration'],
    capabilities: ['opcua-node-explorer'],
  },
  setup(api) {
    api.registerViewer({
      id: 'sensors.opcua-explorer',
      label: 'OPC UA network explorer',
      accepts: ['https://streamfind.dev/catalogue/plugins/sensors#opcuaExploration'],
      component: OpcuaExplorerViewer,
    });
  },
};

export default sensorsFrontendPlugin;
