import { combineReducers, configureStore, PreloadedState } from '@reduxjs/toolkit';
import connectionsReducer from "./slices/connectionsSlice";
import workersReducer from "./slices/workersSlice";
import pipelineInstancesReducer from "./slices/pipelineInstancesSlice";
import segmentInstancesReducer from "./slices/segmentInstancesSlice";

// Create the root reducer separately so we can extract the RootState type
const rootReducer = combineReducers({
   connections: connectionsReducer,
   workers: workersReducer,
   pipelineInstances: pipelineInstancesReducer,
   segmentInstances: segmentInstancesReducer,
});

export const setupStore = (preloadedState?: PreloadedState<RootState>) => {
   return configureStore({
      reducer: rootReducer,
      preloadedState,
   });
};

const rootStore = setupStore();

export function getRootStore() {
   return rootStore;
}

// Infer the `RootState` and `AppDispatch` types from the store itself
export type RootStore = ReturnType<typeof setupStore>;
export type RootState = ReturnType<typeof rootReducer>;
export type AppDispatch = typeof rootStore.dispatch;
export type AppGetState = typeof rootStore.getState;
