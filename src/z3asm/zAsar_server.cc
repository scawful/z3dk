#include <grpcpp/grpcpp.h>

#include <iostream>

#include "interface-lib.h"  // Include Asar interface
#include "zasar.grpc.pb.h"  // Generated from the .proto file

class zAsarServiceImpl final : public zasar::zAsarService::Service {
 public:
  grpc::Status Patch(grpc::ServerContext* context,
                     const zasar::PatchRequest* request,
                     zasar::PatchResponse* response) override {
    int error_count = 0;

    // Patch logic using asar_patch()
    patchparams params;
    params.patchloc = request->asm_file().c_str();

    return grpc::Status::OK;
  }

  grpc::Status GetDiagnostics(grpc::ServerContext* context,
                              const zasar::PatchRequest* request,
                              zasar::PatchResponse* response) override {
    // Diagnostics logic using asar_geterrors()
    return Patch(context, request, response);
  }
};

void RunServer() {
  std::string server_address("0.0.0.0:50051");
  zAsarServiceImpl service;

  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  server->Wait();
}

int main(int argc, char** argv) {
  RunServer();
  return 0;
}
