#include "wasm/wasm_context.hpp"
#include "wasm/wasm_native_contract.hpp"
#include "wasm/types/name.hpp"
#include "wasm/wasm_constants.hpp"
#include "wasm/wasm_log.hpp"
#include "entities/account.h"

#include "wasm/exception/exceptions.hpp"

using namespace std;
using namespace wasm;
// using std::chrono::microseconds;
// using std::chrono::system_clock;

namespace wasm {
    using nativeHandler = std::function<void(wasm_context & )>;
    map <pair<uint64_t, uint64_t>, nativeHandler>& get_wasm_native_handlers(){
        static map <pair<uint64_t, uint64_t>, nativeHandler> wasm_native_handlers;
        return wasm_native_handlers;

    }

    inline void register_native_handler(uint64_t receiver, uint64_t action, nativeHandler v) {
        get_wasm_native_handlers()[std::pair(receiver, action)] = v;
    }

    inline nativeHandler *find_native_handle(uint64_t receiver, uint64_t action) {
        auto handler = get_wasm_native_handlers().find(std::pair(receiver, action));
        if (handler != get_wasm_native_handlers().end()) {
            return &handler->second;
        }

        return nullptr;
    }

    static inline void print_debug(uint64_t receiver, const inline_transaction_trace &trace) {
        if (!trace.console.empty()) {

            ostringstream prefix;

            auto contract_s = name(trace.trx.contract).to_string();
            auto action_s   = name(trace.trx.action).to_string();
            auto receiver_s = name(receiver).to_string();

            prefix << "[" << contract_s << "," << action_s << "]->" << receiver_s << "";
            std::cout << prefix.str()  << ": CONSOLE OUTPUT BEGIN =====================\n"
                      << trace.console << "\n"
                      << prefix.str()  << ": CONSOLE OUTPUT END   =====================\n\n";
        }
    }


    void wasm_context::reset_console() {
        _pending_console_output = std::ostringstream();
    }

    bool wasm_context::has_permission_from_inline_transaction(const permission& p){
        return std::find(trx.authorization.begin(), trx.authorization.end(), p) != trx.authorization.end();
    }

    void wasm_context::execute_inline(const inline_transaction& t) {

        //contract-self or wasmio_bank
        CHAIN_ASSERT( t.contract == _receiver || t.contract == wasmio_bank ,
                      wasm_chain::missing_auth_exception,
                     "Inline transaction can be sent to/by contract self or wasmio.bank ");

        //check authorization
        for(const auto p: t.authorization){

            //inline wasmio.bank
            if(t.contract == wasmio_bank && p.account != _receiver){
                CHAIN_ASSERT( false,
                              wasm_chain::missing_auth_exception,
                              "Inline to wasmio.bank can be only authorized by contract-self %s, but get %s",
                              wasm::name(_receiver).to_string(), wasm::name(p.account).to_string());
            }

            //call contract-self and authorized by contract
            if(t.contract == _receiver && p.account == _receiver ) continue;

            //call contract-self
            if(t.contract == _receiver && !has_permission_from_inline_transaction(p) ) {
                CHAIN_ASSERT( false,
                              wasm_chain::missing_auth_exception,
                              "Missing authorization by account %s in a new inline  transaction",
                              wasm::name(p.account).to_string());
            }

            //call another contract
            // if(t.contract != _receiver && p.account == _receiver && p.perm == wasmio_code){
            //     WASM_ASSERT(false,
            //                 missing_auth_exception,
            //                 "Inline to another contract can be only authorized by contract-self %s in wasmio.code, but get %s",
            //                 wasm::name(_receiver).to_string(), wasm::name(p.account).to_string());
            // }

        }

        inline_transactions.push_back(t);
    }

    std::vector <uint8_t> wasm_context::get_code(const uint64_t& account) {

        vector <uint8_t>   code;
        CUniversalContract contract;
        CAccount contract_account ;
        if(database.accountCache.GetAccount(CNickID(account), contract_account)
            && database.contractCache.GetContract(contract_account.regid, contract)) {
            code = vector <uint8_t>(contract.code.begin(), contract.code.end());
        }
        return code;
    }

    // std::string wasm_context::get_abi(uint64_t account) {
    //     CUniversalContract contract;
    //     CAccount contract_account ;
    //     database.accountCache.GetAccount(CNickID(wasm::name(account).to_string()),contract_account);
    //     database.contractCache.GetContract(contract_account.regid, contract);
    //     return contract.abi;
    // }

    void wasm_context::initialize() {

        static bool wasm_interface_inited = false;
        if (!wasm_interface_inited) {
            wasm_interface_inited = true;
            wasmif.initialize(wasm::vm_type::eos_vm_jit);
            register_native_handler(wasmio,      N(setcode),  wasmio_native_setcode      );
            register_native_handler(wasmio_bank, N(transfer), wasmio_bank_native_transfer);
        }
    }

    void wasm_context::execute(inline_transaction_trace &trace) {

        initialize();

        notified.push_back(_receiver);
        execute_one(trace);

        for (uint32_t i = 1; i < notified.size(); ++i) {
            _receiver = notified[i];

            trace.inline_traces.emplace_back();
            execute_one(trace.inline_traces.back());
        }

        CHAIN_ASSERT( recurse_depth < wasm::max_inline_transaction_depth,
                      wasm_chain::transaction_exception,
                      "max inline transaction depth per transaction reached");

        for (auto &inline_trx : inline_transactions) {
            trace.inline_traces.emplace_back();
            control_trx.execute_inline_transaction(trace.inline_traces.back(), inline_trx,
                                                   inline_trx.contract, database, receipts,
                                                   recurse_depth + 1);
        }

    }

    void wasm_context::execute_one(inline_transaction_trace &trace) {

        //auto start = system_clock::now();
        control_trx.recipients_size ++;

        trace.trx      = trx;
        trace.receiver = _receiver;

        auto native    = find_native_handle(_receiver, trx.action);

        //reset_console();
        try {
            if (native) {
                (*native)(*this);
            } else {
                vector <uint8_t> code = get_code(_receiver);
                if (code.size() > 0) {
                    wasmif.execute(code, this);
                }
            }
        }  catch (wasm_chain::exception &e) {
            string console_output = (_pending_console_output.str().size() == 0)?string(""):string(", console: ") + _pending_console_output.str();
            CHAIN_RETHROW_EXECPTION( e, log_level::warn, 
                                     "[%s, %s]->%s%s",
                                     name(contract()).to_string(),
                                     name(action()).to_string(),
                                     name(receiver()).to_string(),
                                     console_output );
        } catch (...) {
            string console_output = (_pending_console_output.str().size() == 0)?string(""):string(", console: ") + _pending_console_output.str();
            CHAIN_THROW( wasm_chain::chain_exception, 
                         "[%s, %s]->%s%s", 
                         name(contract()).to_string(),
                         name(action()).to_string(),
                         name(receiver()).to_string(),
                         console_output );
        }

        trace.trx_id  = control_trx.GetHash();
        trace.console = _pending_console_output.str();
        //trace.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(system_clock::now() - start);

        reset_console();

        if (contracts_console()) {
            print_debug(_receiver, trace);
        }

    }

    bool wasm_context::has_recipient(const uint64_t& account) const {
        for (auto a : notified)
            if (a == account)
                return true;
        return false;
    }

    void wasm_context::require_recipient(const uint64_t& recipient)  {

        if (!has_recipient(recipient)) {
            notified.push_back(recipient);
        }

        CHAIN_ASSERT( notified.size() <= max_recipients_size, 
                      wasm_chain::recipients_size_exceeds_exception, 
                      "recipients size must be <= '%ld', but get '%ld'", max_recipients_size, notified.size() );

    }

    void wasm_context::require_auth( const uint64_t& account ) const {
        for(auto p: trx.authorization){
            if(p.account == account){
                return;
            }
        }
        CHAIN_ASSERT(false, wasm_chain::missing_auth_exception, "missing authority of %s", wasm::name(account).to_string());
    }

    bool wasm_context::has_authorization( const uint64_t& account ) const {
        for(auto p: trx.authorization){
            if(p.account == account){
                return true;
            }
        }

        return false;
    }

    bool wasm_context::is_account( const uint64_t& account ) const {

        //auto account_name = wasm::name(account);
        return database.accountCache.HasAccount(nick_name(account));
    }

    std::vector<uint64_t> wasm_context::get_active_producers(){

        auto &database_account  = database.accountCache;
        auto &database_delegate = database.delegateCache;

        std::vector<uint64_t> active_producers;
        VoteDelegateVector    producers;
        CHAIN_ASSERT( database_delegate.GetActiveDelegates(producers), 
                      wasm_chain::account_access_exception, 
                      "fail to get top delegates for active producer");

        for( auto p: producers){
            CAccount producer;
            CHAIN_ASSERT( database_account.GetAccount(p.regid, producer),
                          wasm_chain::account_access_exception, 
                          "producer account get account error, regid = %s",
                          p.regid.ToString())  

            CHAIN_ASSERT( producer.nickid.value != 0,
                          wasm_chain::account_access_exception, 
                          "producer account does not register nick_id, regid = %s",
                          p.regid.ToString())  

            active_producers.push_back(producer.nickid.value);          
        }
        return active_producers;
    }

    void wasm_context::update_storage_usage(const uint64_t& account, const int64_t& size_in_bytes){

        int64_t disk_usage    = size_in_bytes * store_fuel_fee_per_byte;
        control_trx.run_cost += (disk_usage < 0) ? 0 : disk_usage;
    }

}