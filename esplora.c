#include <bitcoin/base58.h>
#include <bitcoin/block.h>
#include <bitcoin/feerate.h>
#include <bitcoin/script.h>
#include <bitcoin/shadouble.h>
#include <ccan/array_size/array_size.h>
#include <ccan/cast/cast.h>
#include <ccan/io/io.h>
#include <ccan/json_out/json_out.h>
#include <ccan/pipecmd/pipecmd.h>
#include <ccan/str/hex/hex.h>
#include <ccan/take/take.h>
#include <ccan/tal/grab_file/grab_file.h>
#include <ccan/tal/path/path.h>
#include <ccan/tal/str/str.h>
#include <common/json_helpers.h>
#include <common/memleak.h>
#include <common/utils.h>
#include <errno.h>
#include <inttypes.h>
#include <plugins/libplugin.h>
#include <curl/curl.h>

static char *endpoint = NULL;
static char *blockchair_endpoint = NULL;
static char *cainfo_path = NULL;
static u64 verbose = 0;

struct curl_memory_data {
  char *memory;
  size_t size;
};

static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  size_t realsize = size * nmemb;
  struct curl_memory_data *mem = (struct curl_memory_data *)userp;
 
  char *ptr = realloc(mem->memory, mem->size + realsize + 1);
  if(ptr == NULL) {
    /* out of memory! */ 
    printf("not enough memory (realloc returned NULL)\n");
    return 0;
  }
 
  mem->memory = ptr;
  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;
 
  return realsize;
}

static char *request(const char *url, const bool post, const char* data) {
	struct curl_memory_data chunk;
	chunk.memory = malloc(64);
	chunk.size = 0;

	CURL *curl;
	CURLcode res;
	curl = curl_easy_init();
	if (!curl) {
	    return NULL;
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
	if (verbose != 0)
		curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
	if (cainfo_path != NULL)
		curl_easy_setopt(curl,CURLOPT_CAINFO, cainfo_path);
	if (post) {
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
	}
	curl_easy_setopt(curl, CURLOPT_CAPATH, "/system/etc/security/cacerts");
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);

	res = curl_easy_perform(curl);
	if(res != CURLE_OK) {
		return NULL;
	}
	long response_code;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
	if (response_code != 200) {
		return NULL;
	}
	curl_easy_cleanup(curl);
	return chunk.memory;
}

static char *request_get(const char *url) {
	return request(url, false, NULL);
}

static char *request_post(const char *url, const char* data) {
	return request(url, true, data);
}

static char* get_network_from_genesis_block(const char *blockhash) {
	if (strcmp(blockhash, "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f") == 0)
		return "main";
	else if (strcmp(blockhash, "000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943") == 0)
		return "test";
	else if (strcmp(blockhash, "0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206") == 0)
		return "regtest";
	else
		return NULL;
}

/* Get infos about the block chain.
 * Calls `getblockchaininfo` and returns headers count, blocks count,
 * the chain id, and whether this is initialblockdownload.
 */
static struct command_result *getchaininfo(struct command *cmd,
                                           const char *buf UNUSED,
                                           const jsmntok_t *toks UNUSED)
{
	char *err;

	if (!param(cmd, buf, toks, NULL))
	    return command_param_failed();

	plugin_log(cmd->plugin, LOG_INFORM, "getchaininfo");

	// fetch block genesis hash
	const char *block_genesis_url = tal_fmt(cmd->plugin, "%s/block-height/0", endpoint);
	const char *block_genesis = request_get(block_genesis_url);
	if (!block_genesis) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, block_genesis_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	plugin_log(cmd->plugin, LOG_INFORM, "block_genesis: %s", block_genesis);

	// fetch block count
	const char *blockcount_url = tal_fmt(cmd->plugin, "%s/blocks/tip/height", endpoint);
	const char *blockcount = request_get(blockcount_url);
	if (!blockcount) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, blockcount_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	plugin_log(cmd->plugin, LOG_INFORM, "blockcount: %s", blockcount);
	
	char *eptr;
	const u32 height = strtoul(blockcount, &eptr, 10);
	if (!height) {
		err = tal_fmt(cmd, "%s: invalid height conversion on %s", cmd->methodname, blockcount);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// parsing blockgenesis to get the chain name information
	const char *chain = get_network_from_genesis_block(block_genesis);
	if (!chain) {
		err = tal_fmt(cmd, "%s: no chain found for genesis block %s", cmd->methodname, block_genesis);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// send response with chain information
	struct json_stream *response = jsonrpc_stream_success(cmd);
	json_add_string(response, "chain", chain);
	json_add_u32(response, "headercount", height);
	json_add_u32(response, "blockcount", height);
	json_add_bool(response, "ibd", false);

	return command_finished(cmd, response);
}

static struct command_result *
getrawblockbyheight_notfound(struct command *cmd)
{
	struct json_stream *response;

	response = jsonrpc_stream_success(cmd);
	json_add_null(response, "blockhash");
	json_add_null(response, "block");

	return command_finished(cmd, response);
}

/* Get a raw block given its height.
 * Calls `getblockhash` then `getblock` to retrieve it from bitcoin_cli.
 * Will return early with null fields if block isn't known (yet).
 */
static struct command_result *getrawblockbyheight(struct command *cmd,
                                                  const char *buf,
                                                  const jsmntok_t *toks)
{
	struct json_stream *response;
	const jsmntok_t *tokens, *blocktok;
	u32 *height;
	bool valid;
	char *err;

	if (!param(cmd, buf, toks,
	           p_req("height", param_number, &height),
	           NULL))
		return command_param_failed();

	plugin_log(cmd->plugin, LOG_INFORM, "getrawblockbyheight %d", *height);

	// fetch blockhash from block height
	const char *blockhash_url = tal_fmt(cmd->plugin, "%s/block-height/%d", endpoint, *height);
	const char *blockhash = request_get(blockhash_url);
	if (!blockhash) {
		// block not found as getrawblockbyheight_notfound
		return getrawblockbyheight_notfound(cmd);
	}
	plugin_log(cmd->plugin, LOG_INFORM, "blockhash: %s from %s", blockhash, blockhash_url);

	// Esplora doesn't serve raw blocks !
	// Open issue from darosior: https://github.com/Blockstream/esplora/issues/171
	const char *block_url = tal_fmt(cmd->plugin, 
		"%s/raw/block/%s", blockchair_endpoint, blockhash);
	const char *block_res = request_get(block_url);
	if (!block_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, block_url);
		plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
		// block not found as getrawblockbyheight_notfound
		return getrawblockbyheight_notfound(cmd);
	}

	// parse rawblock output
	tokens = json_parse_input(cmd, block_res,
				  strlen(block_res), &valid);
	if (!tokens) {
		err = tal_fmt(cmd, "%s: json error on %s (%.*s)?",
					cmd->methodname, block_url, (int)sizeof(block_res),
					block_res);
		plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// get rawblock field
	blocktok = json_delve(block_res, tokens, tal_fmt(cmd->plugin, 
		".data.%s.raw_block", blockhash));
	if (!blocktok) {
		err = tal_fmt(cmd,"%s: had no rawblock for block %s from %s (%.*s)?",
			      cmd->methodname, blockhash, block_url, (int)sizeof(block_res),
			      block_res);
		plugin_log(cmd->plugin, LOG_INFORM, "%s", err);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	const char *rawblock = json_strdup(cmd, block_res, blocktok);
	//plugin_log(cmd->plugin, LOG_INFORM, "rawblock: %s", rawblock);

	// send response with block and blockhash in hex format
	response = jsonrpc_stream_success(cmd);
	json_add_string(response, "blockhash", blockhash);
	json_add_string(response, "block", rawblock);

	return command_finished(cmd, response);
}

/* Get current feerate.
 * Calls `estimatesmartfee` and returns the feerate as btc/k*VBYTE*.
 */
static struct command_result *getfeerate(struct command *cmd,
                                         const char *buf UNUSED,
                                         const jsmntok_t *toks UNUSED)
{
	const char *mode;
	char *err;
	u32 *blocks;
	bool valid;
	double feerate = 0;

	if (!param(cmd, buf, toks,
		   p_req("blocks", param_number, &blocks),
		   p_req("mode", param_string, &mode),
		   NULL))
	    return command_param_failed();
	
	if (*blocks == 100)
		*blocks = 144;

	plugin_log(cmd->plugin, LOG_INFORM, "getfeerate for blocks %d", *blocks);

	// fetch feerates
	const char *feerate_url = tal_fmt(cmd->plugin, "%s/fee-estimates", endpoint);
	const char *feerate_res = request_get(feerate_url);
	if (!feerate_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, feerate_url);
		plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	// parse feerates output
	const jsmntok_t *tokens = json_parse_input(cmd, feerate_res,
				  strlen(feerate_res), &valid);
	if (!tokens) {
		err = tal_fmt(cmd, "%s: json error (%.*s)?",
					cmd->methodname, (int)sizeof(feerate_res),
					feerate_res);
		plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	// get feerate for block
	const jsmntok_t *feeratetok = json_get_member(feerate_res, tokens, 
		tal_fmt(cmd->plugin, "%d", *blocks));
	if (!feeratetok || !json_to_double(feerate_res, feeratetok, &feerate)) {
		err = tal_fmt(cmd,"%s: had no feerate for block %d (%.*s)?",
			      cmd->methodname, (int)*blocks, (int)sizeof(feerate_res),
			      feerate_res);
		plugin_log(cmd->plugin, LOG_INFORM, "err: %s", err);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	plugin_log(cmd->plugin, LOG_INFORM, "feerate: %f", feerate);

	// send feerate in response
	struct json_stream *response = jsonrpc_stream_success(cmd);
	json_add_u64(response, "feerate", feerate * 100000);

	return command_finished(cmd, response);
}

static struct command_result *getutxout(struct command *cmd,
                                       const char *buf,
                                       const jsmntok_t *toks)
{
	struct json_stream *response;
	const char *txid, *vout;
	char *err;
	bool spent = false;
	jsmntok_t *tokens;
	struct bitcoin_tx_output output;
	bool valid = false;

	plugin_log(cmd->plugin, LOG_INFORM, "getutxout");

	/* bitcoin-cli wants strings. */
	if (!param(cmd, buf, toks,
	           p_req("txid", param_string, &txid),
	           p_req("vout", param_string, &vout),
	           NULL))
		return command_param_failed();

	// convert vout to number
	char *eptr;
	const u32 vout_index = strtoul(vout, &eptr, 10);
	if (!vout_index) {
		plugin_log(cmd->plugin, LOG_INFORM, "Conversion error occurred on %s", vout);
	    return command_param_failed();
	}

	// check transaction output is spent
	const char *status_url = tal_fmt(cmd->plugin, "%s/tx/%s/outspend/%s", endpoint, txid, vout);
	const char *status_res = request_get(status_url);
	if (!status_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, status_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	tokens = json_parse_input(cmd, status_res,
				  strlen(status_res), &valid);
	if (!tokens || !valid) {
		err = tal_fmt(cmd, "%s: json error (%.*s)?",
					cmd->methodname, (int)sizeof(status_res),
					status_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// parsing spent field
	const jsmntok_t *spenttok = json_get_member(status_res, tokens, "spent");
	if (!spenttok || !json_to_bool(status_res, spenttok, &spent)) {
		err = tal_fmt(cmd,"%s: had no spent (%.*s)?",
			      cmd->methodname, (int)sizeof(status_res), status_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	/* As of at least v0.15.1.0, bitcoind returns "success" but an empty
	   string on a spent txout. */
	if (spent) {
		response = jsonrpc_stream_success(cmd);
		json_add_null(response, "amount");
		json_add_null(response, "script");
		return command_finished(cmd, response);
	}

	// get transaction information
	const char *gettx_url = tal_fmt(cmd->plugin, "%s/tx/%s", endpoint, txid);
	const char *gettx_res = request_get(gettx_url);
	if (!gettx_res) {
		err = tal_fmt(cmd, "%s: request error on %s", cmd->methodname, gettx_url);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	tokens = json_parse_input(cmd, gettx_res,
				  strlen(gettx_res), &valid);
	if (!tokens || !valid) {
		err = tal_fmt(cmd, "%s: json error (%.*s)?", 
					cmd->methodname, (int)sizeof(gettx_res), 
					gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// parsing vout array field
	const jsmntok_t *vouttok = json_get_member(gettx_res, tokens, "vout");
	if (!vouttok) {
		err = tal_fmt(cmd,"%s: had no vout (%.*s)?",
			      cmd->methodname, (int)sizeof(gettx_res),
			      gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	const jsmntok_t *v = json_get_arr(vouttok, vout_index);
	if (!v) {
		err = tal_fmt(cmd,"%s: had no vout[%d] (%.*s)?",
			      cmd->methodname, (int)vout_index, (int)sizeof(gettx_res),
			      gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// parsing amount value
	const jsmntok_t *valuetok = json_get_member(gettx_res, v, "value");
	if (!valuetok || !json_to_bitcoin_amount(gettx_res, valuetok, &output.amount.satoshis)) { /* Raw: talking to bitcoind */
		err = tal_fmt(cmd,"%s: had no vout[%d] value (%.*s)?",
			      cmd->methodname, vout_index, (int)sizeof(gettx_res),
			      gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// parsing scriptpubkey
	const jsmntok_t *scriptpubkeytok = json_get_member(gettx_res, v, "scriptpubkey");
	if (!scriptpubkeytok) {
		err = tal_fmt(cmd,"%s: had no vout[%d] scriptpubkey (%.*s)?",
			      cmd->methodname, vout_index, (int)sizeof(gettx_res),
			      gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}
	output.script = tal_hexdata(cmd, gettx_res + scriptpubkeytok->start,
	                            scriptpubkeytok->end - scriptpubkeytok->start);
	if (!output.script) {
		err = tal_fmt(cmd, "%s: scriptpubkey invalid hex (%.*s)?",
			      cmd->methodname, (int)sizeof(gettx_res),
			      gettx_res);
		return command_done_err(cmd, BCLI_ERROR, err, NULL);
	}

	// replay response
	response = jsonrpc_stream_success(cmd);
	json_add_amount_sat_only(response, "amount", output.amount);
	json_add_string(response, "script",
			tal_hexstr(response, output.script, sizeof(output.script)));

	return command_finished(cmd, response);
}

/* Send a transaction to the Bitcoin network.
 * Calls `sendrawtransaction` using the first parameter as the raw tx.
 */
static struct command_result *sendrawtransaction(struct command *cmd,
                                                 const char *buf,
                                                 const jsmntok_t *toks)
{
	const char *tx;

	/* bitcoin-cli wants strings. */
	if (!param(cmd, buf, toks,
	           p_req("tx", param_string, &tx),
	           NULL))
		return command_param_failed();

	plugin_log(cmd->plugin, LOG_INFORM, "sendrawtransaction");
	
	// request post passing rawtransaction
	const char *sendrawtx_url = tal_fmt(cmd->plugin, "%s/tx", endpoint);
	const char *res = request_post(sendrawtx_url, tx);
	struct json_stream *response = jsonrpc_stream_success(cmd);
	if (!res) {
		// send response with failure
		const char *err = tal_fmt(cmd, "%s: invalid tx (%.*s)? on (%.*s)?",
			      cmd->methodname,
				  (int)sizeof(tx), tx,
				  (int)sizeof(sendrawtx_url), sendrawtx_url);
		json_add_bool(response, "success", false);
		json_add_string(response, "errmsg", err);
	}

	// send response with success
	json_add_bool(response, "success", true);
	json_add_string(response, "errmsg", "");
	return command_finished(cmd, response);
}

static void init(struct plugin *p, const char *buffer UNUSED,
                 const jsmntok_t *config UNUSED)
{
	plugin_log(p, LOG_INFORM, "esplora initialized.");
}

static const struct plugin_command commands[] = {
	{
		"getrawblockbyheight",
		"bitcoin",
		"Get the bitcoin block at a given height",
		"",
		getrawblockbyheight
	},
	{
		"getchaininfo",
		"bitcoin",
		"Get the chain id, the header count, the block count,"
		" and whether this is IBD.",
		"",
		getchaininfo
	},
	{
		"getfeerate",
		"bitcoin",
		"Get the Bitcoin feerate in btc/kilo-vbyte.",
		"",
		getfeerate
	},
	{
		"sendrawtransaction",
		"bitcoin",
		"Send a raw transaction to the Bitcoin network.",
		"",
		sendrawtransaction
	},
	{
		"getutxout",
		"bitcoin",
		"Get informations about an output, identified by a {txid} an a {vout}",
		"",
		getutxout
	},
};

int main(int argc, char *argv[])
{
	setup_locale();

	plugin_main(argv, init, PLUGIN_STATIC, commands, ARRAY_SIZE(commands),
		    NULL, 0, NULL, 0,
		    plugin_option("esplora-api-endpoint",
				  "string",
				  "The URL of the esplora instance to hit (including '/api').",
				  charp_option, &endpoint),
		    plugin_option("blockchair-api-endpoint",
				  "string",
				  "Select the blockchair api url only to fetch rawblocks.",
				  charp_option, &blockchair_endpoint),
		    plugin_option("esplora-cainfo",
				  "string",
				  "Set path to Certificate Authority (CA) bundle.",
				  charp_option, &cainfo_path),
		    plugin_option("esplora-verbose",
				  "int",
				  "Set verbose output (default 0).",
				  u64_option, &verbose),
		    NULL);
}
