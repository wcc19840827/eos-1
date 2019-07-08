#include <appbase/application.hpp>

#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>
#include <eosio/chain_api_plugin/chain_api_plugin.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/producer_plugin/producer_plugin.hpp>

#include <boost/program_options.hpp>
#include <boost/algorithm/string/join.hpp>

using namespace appbase;
using namespace eosio;
using namespace eosio::chain;
using namespace std;

namespace bpo = boost::program_options;
using bpo::options_description;
using bpo::variables_map;

enum return_codes {
   OTHER_FAIL        = -2,
   INITIALIZE_FAIL   = -1,
   SUCCESS           = 0,
};

const fc::microseconds abi_serializer_max_time{1000*1000};
const uint32_t default_billed_cpu_time_us = 2000;


vector<string> get_producers_name_from_snapshot( const string& snapshot_path ) {
   vector<string> producers_name;

   // Read producer name list from genesis
   auto infile = std::ifstream(snapshot_path, (std::ios::in | std::ios::binary));
   auto reader = std::make_shared<istream_snapshot_reader>(infile);
   reader->validate();
   reader->read_section<block_state>([&]( auto &section ){
      block_header_state head_header_state;
      section.read_row(head_header_state);
      for (auto& prod: head_header_state.active_schedule.producers ) {
         producers_name.emplace_back( prod.producer_name.to_string() );
      }
   });
   infile.close();
   return producers_name;

}

void add_block_with_regproducer_trxs( controller& chain, const private_key_type& priv_key, const public_key_type& pub_key ) {
   auto abi = chain.get_account(N(eosio)).get_abi();
   chain::abi_serializer abis(abi, abi_serializer_max_time);
   string action_type_name = abis.get_action_type(N(regproducer));
   FC_ASSERT( action_type_name != string(), "regproducer action is not found");

   std::cerr << "ADD BLOCK WITH REGPRODUCER" << chain.head_block_num() << std::endl;
   auto next_time = chain.head_block_time() + fc::milliseconds(config::block_interval_ms);
   chain.start_block( next_time, chain.head_block_num() - chain.last_irreversible_block_num(), {} );

   for (auto& prod: chain.head_block_state()->active_schedule.producers ) {
      ilog("Call regproducer for ${n}", ("n", prod.producer_name));
      action act;
      act.account = N(eosio);
      act.name = N(regproducer);
      act.authorization = {{prod.producer_name, config::active_name}};
      act.data = abis.variant_to_binary( action_type_name,  
                                       mutable_variant_object()
                                          ("producer",      prod.producer_name)
                                          ("producer_key",  string(pub_key))
                                          ("url",           "")
                                          ("location",      0), 
                                       abi_serializer_max_time);

      signed_transaction trx;
      trx.actions.emplace_back( act );
      trx.expiration = next_time + fc::seconds(300);
      trx.set_reference_block( chain.head_block_id() );
      trx.max_net_usage_words = 0; 
      trx.max_cpu_usage_ms = 0;
      trx.delay_sec = 0;
      trx.sign( priv_key, chain.get_chain_id() );

      auto mtrx = std::make_shared<transaction_metadata>(trx);
      transaction_metadata::start_recover_keys( mtrx, chain.get_thread_pool(), chain.get_chain_id(), fc::microseconds::maximum() );
      auto result = chain.push_transaction( mtrx, fc::time_point::maximum(), default_billed_cpu_time_us );
      if( result->except_ptr ) std::rethrow_exception( result->except_ptr );
      if( result->except)  throw *result->except;
   }

   chain.finalize_block( [&]( const digest_type& d ) {
      return priv_key.sign(d);
   } );

   chain.commit_block();
}

int main(int argc, char** argv)
{
   try {
      http_plugin::set_defaults({
         .default_unix_socket_path = "",
         .default_http_port = 8888
      });

      auto priv_key = private_key_type::regenerate<fc::ecc::private_key_shim>(fc::sha256::hash(std::string("hahaha")));
      auto pub_key = priv_key.get_public_key();

      string snapshot_path = "/Users/andrianto.lie/Downloads/snapshot-00000025befd8bf88dc052aa016fe55963cbeb7e5f6e4e62f8868ef2596187ae.bin";
      string snapshot_output_path = "/Users/andrianto.lie/Downloads/snapshot-haha.bin";

      auto root = fc::app_path();
      app().set_default_data_dir(root / "eosio" / "snapshot-modifier" / "data" );
      app().set_default_config_dir(root / "eosio" / "snapshot-modifier" / "config" );

      vector<const char*> custom_argv;
      custom_argv.emplace_back(argv[0]);
      // custom_argv.emplace_back("--plugin");
      // custom_argv.emplace_back("--eosio::chain_api_plugin");
      // custom_argv.emplace_back("--pause-on-startup");
      custom_argv.emplace_back("--delete-all");
      custom_argv.emplace_back("--snapshot");
      custom_argv.emplace_back(snapshot_path.c_str());
      // custom_argv.emplace_back("-e");
      // auto producers_name = get_producers_name_from_snapshot(snapshot_path);
      // for (auto& prod_name: producers_name) {
      //    std::cerr << "PROD NAME" << prod_name << std::endl;
      //    custom_argv.emplace_back("-p");
      //    custom_argv.emplace_back(prod_name.c_str());
      // }
      // custom_argv.emplace_back("--signature-provider");
      string signature_provider = string(pub_key) + "=KEY:" + string(priv_key);
       std::cerr << "SIGN PROVIDER NAME" << signature_provider << std::endl;
      // custom_argv.emplace_back(signature_provider.c_str());
      // std::string joinedString = boost::algorithm::join(custom_argv, ",");
      // std::cerr << joinedString << std::endl;
      if(!app().initialize<chain_plugin, chain_api_plugin, producer_plugin>(custom_argv.size(), const_cast<char**>(custom_argv.data()))) {
         return INITIALIZE_FAIL;
      }
      app().startup();

      // Replace producer keys
      auto chain_plug = app().find_plugin<chain_plugin>();
      auto prod_plug = app().find_plugin<producer_plugin>();
      chain::controller& chain = chain_plug->chain();
      chain.replace_producer_keys( pub_key );

      // Call regproducer for each producer
      add_block_with_regproducer_trxs( chain, priv_key, pub_key );



      // create the snapshot
      auto snap_out = std::ofstream(snapshot_output_path, (std::ios::out | std::ios::binary));
      auto writer = std::make_shared<ostream_snapshot_writer>(snap_out);
      chain.write_snapshot(writer);
      writer->finalize();
      snap_out.flush();
      snap_out.close();

      app().shutdown();
     
       std::cerr << "SNAPSHOT WRITTEN SYNC" << std::endl;
   }  catch( const fc::exception& e ) {
      elog( "${e}", ("e", e.to_detail_string()));
      return OTHER_FAIL;
   } catch( const std::exception& e ) {
      elog("Caught Exception: ${e}", ("e",e.what()));
      return OTHER_FAIL;
   } catch( ... ) {
      elog("Unknown exception");
      return OTHER_FAIL;
   }

   ilog("${name} successfully exiting", ("name", "eosio-snapshot-modifier"));
   return SUCCESS;
}
