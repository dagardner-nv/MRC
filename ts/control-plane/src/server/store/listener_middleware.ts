import {addListener, createListenerMiddleware} from "@reduxjs/toolkit";

import type {TypedStartListening, TypedAddListener} from "@reduxjs/toolkit";

import type {RootState, AppDispatch} from "./store";

export const listenerMiddleware = createListenerMiddleware<RootState>();

export type AppStartListening = TypedStartListening<RootState, AppDispatch>;

export const startAppListening = listenerMiddleware.startListening as AppStartListening;

export const addAppListener = addListener as TypedAddListener<RootState, AppDispatch>;
