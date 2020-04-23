import {VizierQueryError} from 'common/errors';
import * as ls from 'common/localstorage';
import {Table, VizierQueryFunc} from 'common/vizier-grpc-client';
import ClientContext from 'common/vizier-grpc-client-context';
import {SnackbarProvider, useSnackbar} from 'components/snackbar/snackbar';
import {parseSpecs, VisualizationSpecMap} from 'components/vega/spec';
import * as QueryString from 'query-string';
import * as React from 'react';

import {parsePlacementOld, Placement} from './layout';
import {getQueryFuncs, parseVis, Vis} from './vis';

interface LiveContextProps {
  // Old live view format functions.
  updateVegaSpecOld: (spec: VisualizationSpecMap) => void;
  updatePlacementOld: (placement: Placement) => void;
  executeScriptOld: (script?: string) => void;
  setScriptsOld: (script: string, vega: string, placement: string, title: Title) => void;

  // Shared between old and new live view functions.
  updateScript: (code: string) => void;
  vizierReady: boolean;
  toggleDataDrawer: () => void;

  // New live view functions.
  executeScript: (script?: string, vis?: Vis) => void;
  setScripts: (script: string, vis: string, title: Title) => void;
  updateVis: (spec: Vis) => void;

  // Temporary bool that allows the UI to know whether the current live view is old or new mode.
  oldLiveViewMode: boolean;
}

interface Tables {
  [name: string]: Table;
}

interface Results {
  error?: Error;
  tables: Tables;
}

interface Title {
  title: string;
  id: string;
}

export const ScriptContext = React.createContext<string>('');
export const VegaContextOld = React.createContext<VisualizationSpecMap>(null);
export const PlacementContextOld = React.createContext<Placement>(null);
export const ResultsContext = React.createContext<Results>(null);
export const LiveContext = React.createContext<LiveContextProps>(null);
export const TitleContext = React.createContext<Title>(null);
export const VisContext = React.createContext<Vis>(null);
export const DrawerContext = React.createContext<boolean>(false);

const LiveContextProvider = (props) => {
  const [script, setScript] = React.useState<string>(ls.getLiveViewPixieScript());
  React.useEffect(() => {
    ls.setLiveViewPixieScript(script);
  }, [script]);

  const [vegaSpec, setVegaSpecOld] = React.useState<VisualizationSpecMap>(
    parseSpecs(ls.getLiveViewVegaSpecOld()) || {});
  React.useEffect(() => {
    ls.setLiveViewVegaSpecOld(JSON.stringify(vegaSpec, null, 2));
  }, [vegaSpec]);

  const [placement, setPlacementOld] = React.useState<Placement>(
    parsePlacementOld(ls.getLiveViewPlacementSpecOld()) || {});
  React.useEffect(() => {
    ls.setLiveViewPlacementSpecOld(JSON.stringify(placement, null, 2));
  }, [placement]);

  const [results, setResults] = React.useState<Results>({ tables: {} });

  const [vis, setVis] = React.useState<Vis>(parseVis(ls.getLiveViewVisSpec()) || { variables: [], widgets: [] });
  React.useEffect(() => {
    ls.setLiveViewVisSpec(JSON.stringify(vis, null, 2));
  }, [vis]);

  const [oldLiveViewMode, setOldLiveViewMode] = React.useState<boolean>(ls.getOldLiveViewMode());
  React.useEffect(() => {
    ls.setOldLiveViewMode(oldLiveViewMode);
  }, [oldLiveViewMode]);

  const setScriptsOld = React.useCallback((newScript, newVega, newPlacement, newTitle) => {
    setOldLiveViewMode(true);
    setScript(newScript);
    setTitle(newTitle);
    setVegaSpecOld(parseSpecs(newVega) || {});
    setPlacementOld(parsePlacementOld(newPlacement) || {});
  }, []);

  const setScripts = React.useCallback((newScript, newVis, newTitle) => {
    setOldLiveViewMode(false);
    setScript(newScript);
    setTitle(newTitle);
    setVis(parseVis(newVis) || { variables: [], widgets: [] });
  }, []);

  const [title, setTitle] = React.useState<Title>(ls.getLiveViewTitle());
  React.useEffect(() => {
    ls.setLiveViewTitle(title);
  }, [title]);

  const [dataDrawerOpen, setDataDrawerOpen] = React.useState<boolean>(ls.getLiveViewDataDrawerOpened());
  const toggleDataDrawer = React.useCallback(() => setDataDrawerOpen((opened) => !opened), []);
  React.useEffect(() => {
    ls.setLiveViewDataDrawerOpened(dataDrawerOpen);
  }, [dataDrawerOpen]);

  const client = React.useContext(ClientContext);

  const showSnackbar = useSnackbar();

  const executeScriptOld = React.useCallback((inputScript?: string) => {
    if (!client) {
      return;
    }

    let errMsg: string;
    let queryId: string;

    client.executeScriptOld(inputScript || script).then((queryResults) => {
      const newTables = {};
      queryId = queryResults.queryId;
      for (const table of queryResults.tables) {
        newTables[table.name] = table;
      }
      setResults({ tables: newTables });
    }).catch((error) => {
      const errType = (error as VizierQueryError).errType;
      errMsg = error.message;
      if (errType === 'execution') {
        showSnackbar({
          message: errMsg,
          action: () => setDataDrawerOpen(true),
          actionTitle: 'details',
          autoHideDuration: 5000,
        });
      } else {
        showSnackbar({
          message: errMsg,
          action: () => executeScript(inputScript),
          actionTitle: 'retry',
          autoHideDuration: 5000,
        });
      }
      setResults({ tables: {}, error });
    }).finally(() => {
      analytics.track('Query Execution', {
        status: errMsg ? 'success' : 'failed',
        query: script,
        queryID: queryId,
        error: errMsg,
        title,
      });
    });
  }, [client, script, title]);

  const executeScript = React.useCallback((inputScript?: string, inputVis?: Vis) => {
    if (!client) {
      return;
    }

    let errMsg: string;
    let queryId: string;

    new Promise((resolve, reject) => {
      const variableValues = {};
      const parsed = QueryString.parse(location.search);
      Object.keys(parsed).forEach((key) => {
        variableValues[key] = parsed[key] as string;
      });

      try {
        resolve(getQueryFuncs(inputVis || vis, variableValues));
      } catch (error) {
        reject(error);
      }
    })
      .then((funcs: VizierQueryFunc[]) => client.executeScript(inputScript || script, funcs))
      .then((queryResults) => {
        const newTables = {};
        queryId = queryResults.queryId;
        for (const table of queryResults.tables) {
          newTables[table.name] = table;
        }
        setResults({ tables: newTables });
      }).catch((error) => {
        const errType = (error as VizierQueryError).errType;
        errMsg = error.message;
        if (errType === 'execution') {
          showSnackbar({
            message: errMsg,
            action: () => setDataDrawerOpen(true),
            actionTitle: 'details',
            autoHideDuration: 5000,
          });
        } else {
          showSnackbar({
            message: errMsg,
            action: () => executeScript(inputScript),
            actionTitle: 'retry',
            autoHideDuration: 5000,
          });
        }
        setResults({ tables: {}, error });
      }).finally(() => {
        analytics.track('Query Execution', {
          status: errMsg ? 'success' : 'failed',
          query: script,
          queryID: queryId,
          error: errMsg,
          title,
        });
      });
  }, [client, script, vis, title]);

  const liveViewContext = React.useMemo(() => ({
    // Old Live View format
    updateVegaSpecOld: setVegaSpecOld,
    updatePlacementOld: setPlacementOld,
    setScriptsOld,

    // Shared between old and new Live View format
    updateScript: setScript,
    vizierReady: !!client,

    // New Live view format
    setScripts,
    executeScriptOld,
    executeScript,
    updateVis: setVis,
    toggleDataDrawer,

    // temporary
    oldLiveViewMode,
  }), [executeScript, client, oldLiveViewMode]);

  return (
    <LiveContext.Provider value={liveViewContext}>
      <TitleContext.Provider value={title}>
        <ScriptContext.Provider value={script}>
          <VegaContextOld.Provider value={vegaSpec}>
            <PlacementContextOld.Provider value={placement}>
              <ResultsContext.Provider value={results}>
                <VisContext.Provider value={vis}>
                  <DrawerContext.Provider value={dataDrawerOpen}>
                    {props.children}
                  </DrawerContext.Provider>
                </VisContext.Provider>
              </ResultsContext.Provider>
            </PlacementContextOld.Provider>
          </VegaContextOld.Provider>
        </ScriptContext.Provider>
      </TitleContext.Provider>
    </LiveContext.Provider>
  );
};

export function withLiveContextProvider(WrappedComponent) {
  return () => (
    <SnackbarProvider>
      <LiveContextProvider>
        <WrappedComponent />
      </LiveContextProvider>
    </SnackbarProvider>
  );
}
