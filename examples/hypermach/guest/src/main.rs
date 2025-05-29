#![no_std]
#![no_main]
extern crate alloc;

use ::alloc::{string::String, vec::Vec};
use alloc::string::ToString;

use hyperlight_common::flatbuffer_wrappers::{
    function_call::FunctionCall,
    function_types::{ParameterType, ParameterValue, ReturnType},
    guest_error::ErrorCode,
    util::get_flatbuffer_result,
};

use ::hyperlight_guest::error::Result;
use hyperlight_guest::{
    error::HyperlightGuestError, guest_function_definition::GuestFunctionDefinition,
    guest_function_register::register_function,
};

fn direct_echo(function_call: &FunctionCall) -> Result<Vec<u8>> {
    if let ParameterValue::VecBytes(data) = function_call.parameters.clone().unwrap()[0].clone() {
        Ok(get_flatbuffer_result::<&[u8]>(data.as_ref()))
    } else {
        Err(HyperlightGuestError::new(
            ErrorCode::GuestFunctionParameterTypeMismatch,
            "Invalid parameters passed to get_size_prefixed_buffer".to_string(),
        ))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn hyperlight_main() {
    let direct_echo_function_definition = GuestFunctionDefinition::new(
        "DirectEcho".to_string(),
        Vec::from(&[ParameterType::VecBytes]),
        ReturnType::VecBytes,
        direct_echo as usize,
    );
    register_function(direct_echo_function_definition);
}

#[unsafe(no_mangle)]
pub fn guest_dispatch_function(function_call: FunctionCall) -> Result<Vec<u8>> {
    let function_name: String = function_call.function_name.clone();
    return Err(HyperlightGuestError::new(
        ErrorCode::GuestFunctionNotFound,
        function_name,
    ));
}
