
void HeadlessBluetoothHandler::onBluetoothAuthenticationResult(
    const aap_protobuf::service::bluetooth::message::BluetoothAuthenticationResult& result) {
    std::cerr << "[aasdk] BT auth result received" << std::endl;
    channel_->receive(shared_from_this());
}
