import "ix/add/asynciterable-operators/first";
import "ix/add/asynciterable-operators/finalize";

import {Channel, credentials} from "@grpc/grpc-js";
import {ConnectivityState} from "@grpc/grpc-js/build/src/connectivity-state";
import {launchDevtoolsCli} from "@mrc/common/dev_tools";
import {WorkerStates} from "@mrc/proto/mrc/protos/architect_state";
import {IPipelineDefinition, pipelineDefinitionsSelectById} from "@mrc/server/store/slices/pipelineDefinitionsSlice";
import {
   ISegmentDefinition,
   ISegmentMapping,
   segmentDefinitionsSelectById,
} from "@mrc/server/store/slices/segmentDefinitionsSlice";
import {as, AsyncIterableX, AsyncSink} from "ix/asynciterable";
import {share} from "ix/asynciterable/operators";
import {createChannel, createClient, waitForChannelReady} from "nice-grpc";

import {packEvent, stringToBytes} from "../common/utils";
import {
   Ack,
   ArchitectClient,
   ArchitectDefinition,
   ClientConnectedResponse,
   Event,
   EventType,
   PingRequest,
   PipelineRequestAssignmentRequest,
   PipelineRequestAssignmentResponse,
   RegisterWorkersRequest,
   RegisterWorkersResponse,
} from "../proto/mrc/protos/architect";
import {ArchitectServer} from "../server/server";
import {connectionsSelectAll, connectionsSelectById, IConnection} from "../server/store/slices/connectionsSlice";
import {pipelineInstancesSelectById} from "../server/store/slices/pipelineInstancesSlice";
import {segmentInstancesSelectByIds} from "../server/store/slices/segmentInstancesSlice";
import {workersSelectById} from "../server/store/slices/workersSlice";
import {RootStore, setupStore} from "../server/store/store";

import {unpack_first_event, unpack_unary_event} from "./utils";

describe("Client", () => {
   let store: RootStore;
   let server: ArchitectServer;
   let client_channel: Channel;
   let client: ArchitectClient;

   beforeEach(async () => {
      const startTime = performance.now();

      store = setupStore();

      // Use localhost:0 to bind to a random port to avoid collisions when testing
      server = new ArchitectServer(store, "localhost:0");

      const port = await server.start();

      client_channel = createChannel(`localhost:${port}`, credentials.createInsecure());

      // Now make the client
      client = createClient(ArchitectDefinition, client_channel);

      // Important to ensure the channel is ready before continuing
      await waitForChannelReady(client_channel, new Date(Date.now() + 1000));

      console.log(`beforeEach took ${performance.now() - startTime} milliseconds`);
   });

   afterEach(async () => {
      const startTime = performance.now();

      client_channel.close();
      await server.stop();
      await server.join();

      console.log(`afterEach took ${performance.now() - startTime} milliseconds`);
   });

   describe("connection", () => {
      it("is ready", () => {
         expect(client_channel.getConnectivityState(true)).toBe(ConnectivityState.READY);
      });

      it("ping", async () => {
         const req = PingRequest.create({
            tag: 1234,
         });

         const resp = await client.ping(req);

         expect(resp.tag).toBe(req.tag);
      });

      it("no connections on start", async () => {
         expect(connectionsSelectAll(store.getState())).toHaveLength(0);
      });

      describe("eventStream", () => {
         let abort_controller: AbortController;
         let send_events: AsyncSink<Event>;
         let recieve_events: AsyncIterableX<Event>;
         let connected_response: ClientConnectedResponse;

         beforeEach(async () => {
            abort_controller = new AbortController();
            send_events      = new AsyncSink<Event>();

            recieve_events = as (client.eventStream(send_events, {
                                   signal: abort_controller.signal,
                                })).pipe(share());

            connected_response = await unpack_first_event<ClientConnectedResponse>(
                recieve_events,
                {predicate: (event) => event.event === EventType.ClientEventStreamConnected});
         });

         afterEach(async () => {
            // Make sure to close down the send stream
            send_events.end();
         });

         it("connect to event stream", async () => {
            let connections: IConnection[];

            // Verify the number of connections is 1
            const connection = connectionsSelectById(store.getState(), connected_response.machineId);

            expect(connection).toBeDefined();
            expect(connection?.id).toBe(connected_response.machineId);

            // Cause a disconnect
            send_events.end();

            recieve_events.finalize(() => {
               connections = connectionsSelectAll(store.getState());

               expect(connections).toHaveLength(0);
            });
         });

         it("abort after connection", async () => {
            try
            {
               for await (const req of recieve_events)
               {
                  abort_controller.abort();

                  send_events.end();
               }
            } catch (error: any)
            {
               expect(error.message).toMatch("The operation has been aborted");
            }
         });

         it("add one worker", async () => {
            const registered_response = await unpack_unary_event<RegisterWorkersResponse>(
                recieve_events,
                send_events,
                packEvent(EventType.ClientUnaryRegisterWorkers, 9876, RegisterWorkersRequest.create({
                   ucxWorkerAddresses: stringToBytes(["test data"]),
                })));

            expect(registered_response.machineId).toBe(connected_response.machineId);

            // Need to do deeper checking here
         });

         it("activate stream", async () => {
            const registered_response = await unpack_unary_event<RegisterWorkersResponse>(
                recieve_events,
                send_events,
                packEvent(EventType.ClientUnaryRegisterWorkers, 9876, RegisterWorkersRequest.create({
                   ucxWorkerAddresses: stringToBytes(["test data"]),
                })));

            const activated_response = await unpack_unary_event<Ack>(
                recieve_events,
                send_events,
                packEvent(EventType.ClientUnaryActivateStream, 2, registered_response));

            // Check to make sure its activated
            const found_worker = workersSelectById(store.getState(), registered_response.instanceIds[0]);

            expect(found_worker?.state).toBe(WorkerStates.Activated);
         });

         describe("pipeline", () => {
            test("request pipeline", async () => {
               const registered_response = await unpack_unary_event<RegisterWorkersResponse>(
                   recieve_events,
                   send_events,
                   packEvent(EventType.ClientUnaryRegisterWorkers, 9876, RegisterWorkersRequest.create({
                      ucxWorkerAddresses: stringToBytes(["test data", "test data 2"]),
                   })));

               const activated_response = await unpack_unary_event<Ack>(
                   recieve_events,
                   send_events,
                   packEvent(EventType.ClientUnaryActivateStream, 2, registered_response));

               const pipeline_definition: IPipelineDefinition = {
                  id: 0,
                  instanceIds: [],
                  segmentIds: [],
               };

               const segment_definitions: ISegmentDefinition[] = [
                  {
                     egressPorts: [],
                     id: 0,
                     ingressPorts: [],
                     instanceIds: [],
                     name: "my_seg",
                     pipelineId: pipeline_definition.id,
                  },
                  {
                     egressPorts: [],
                     id: 0,
                     ingressPorts: [],
                     instanceIds: [],
                     name: "my_seg2",
                     pipelineId: pipeline_definition.id,
                  },
               ];

               const segment_assignments: ISegmentMapping[] = segment_definitions.flatMap((seg_def) => {
                  return {
                     segmentName: seg_def.name,
                     workerIds: registered_response.instanceIds,
                  } as ISegmentMapping;
               });

               // Now request to run a pipeline
               const request_pipeline_response = await unpack_unary_event<PipelineRequestAssignmentResponse>(
                   recieve_events,
                   send_events,
                   packEvent(EventType.ClientUnaryRequestPipelineAssignment,
                             12345,
                             PipelineRequestAssignmentRequest.create({
                                pipeline: pipeline_definition,
                                segments: segment_definitions,
                                assignments: segment_assignments,
                             })));

               // Check the pipeline definition
               const foundPipelineDefinition = pipelineDefinitionsSelectById(
                   store.getState(),
                   request_pipeline_response.pipelineDefinitionId);

               expect(foundPipelineDefinition?.id).toBe(request_pipeline_response.pipelineDefinitionId);

               // Check the segment definitions
               request_pipeline_response.segmentDefinitionIds.forEach((seg_def_id) => {
                  const foundSegmentDefinition = segmentDefinitionsSelectById(store.getState(), seg_def_id);

                  expect(foundSegmentDefinition).toBeDefined();
               });

               // Check pipeline instances
               const foundPipelineInstance = pipelineInstancesSelectById(store.getState(),
                                                                         request_pipeline_response.pipelineInstanceId);

               expect(foundPipelineInstance?.machineId).toBe(connected_response.machineId);
               expect(foundPipelineInstance?.segmentIds).toEqual(request_pipeline_response.segmentInstanceIds);

               // Check segments exist in state
               const foundSegmentInstances = segmentInstancesSelectByIds(store.getState(),
                                                                         request_pipeline_response.segmentInstanceIds);

               expect(foundSegmentInstances)
                   .toHaveLength(segment_assignments.length * registered_response.instanceIds.length);
            });
         });
      });
   });
});

// Disabling this for now since it causes jest to crash when importing a ESM module

// describe("ClientWithDevTools", () => {
//    let store: RootStore;
//    let server: ArchitectServer;
//    let client_channel: Channel;
//    let client: ArchitectClient;

//    beforeEach(async () => {
//       // First create the dev server
//       await launchDevtoolsCli();

//       // Start the store with dev tools enabled
//       store = setupStore(undefined, true);

//       server = new ArchitectServer(store);

//       const port = await server.start();

//       client_channel = createChannel(`localhost:${port}`, credentials.createInsecure());

//       // Now make the client
//       client = createClient(ArchitectDefinition, client_channel);

//       // Important to ensure the channel is ready before continuing
//       await waitForChannelReady(client_channel, new Date(Date.now() + 1000));
//    });

//    it("ping", async () => {
//       const req = PingRequest.create({
//          tag: 1234,
//       });

//       const resp = await client.ping(req);

//       expect(resp.tag).toBe(req.tag);

//       await new Promise(r => setTimeout(r, 2000));
//    });

//    afterEach(async () => {
//       client_channel.close();
//       await server.stop();
//       await server.join();
//    });
// });
