#include "search.h"
#include "Board.h"
#include "History.h"
#include "PieceData.h"
#include "attack.h"
#include "eval.h"
#include "io.h"
#include "magic.h"
#include "makemove.h"
#include "misc.h"
#include "move.h"
#include "movegen.h"
#include "movepicker.h"
#include "ttable.h"
#include <cassert>
#include <cstring>
#include <thread>
#include <vector>
#include "datagen.h"

int CounterMoves[Board_sq_num][Board_sq_num];

//Contains the material Values of the pieces
int PieceValue[15] = { 100, 300, 300, 450, 900, 0,
					  100, 300, 300, 450, 900, 0,0,0,0 };

// IsRepetition handles the repetition detection of a position
static int IsRepetition(const S_Board* pos) {
	for (int index = pos->hisPly - pos->fiftyMove; index < pos->hisPly; index++)
		// if we found the hash key same with a current
		if (pos->history[index].posKey == pos->posKey)
			// we found a repetition
			return TRUE;

	return FALSE;
}

//If we triggered any of the rules that forces a draw or we know the position is a draw return a draw score
static bool IsDraw(const S_Board* pos) {
	// if it's a 3-fold repetition, the fifty moves rule kicked in or there isn't enough material on the board then it's a draw
	if (((IsRepetition(pos)) && pos->ply) || (pos->fiftyMove >= 100) ||
		MaterialDraw(pos)) {
		return true;
	}
	else
	{
		return false;
	}
}

//ClearForSearch handles the cleaning of the post and the info parameters to start search from a clean state
void ClearForSearch(S_Board* pos, S_Stack* ss, S_SearchINFO* info) {
	//For every piece [12] moved to every square [64] we reset the searchHistory value
	for (int index = 0; index < 12; ++index) {
		for (int index2 = 0; index2 < 64; ++index2) {
			ss->searchHistory[index][index2] = 0;
		}
	}

	//Reset the 2 killer moves that are stored for any searched depth
	for (int index = 0; index < 2; ++index) {
		for (int index2 = 0; index2 < MAXDEPTH; ++index2) {
			ss->searchKillers[index][index2] = 0;
		}
	}

	//Clean the Pv array
	for (int index = 0; index < MAXDEPTH + 1; ++index) {
		ss->pvLength[index] = 0;
		for (int index2 = 0; index2 < MAXDEPTH + 1; ++index2) {
			ss->pvArray[index][index2] = NOMOVE;
		}
	}

	//Reset plies and search info
	pos->ply = 0;
	info->starttime = GetTimeMs();
	info->stopped = 0;
	info->nodes = 0;
	info->seldepth = 0;
}

static inline Bitboard AttacksTo(const S_Board* pos, int to, Bitboard occ) {

	//For every piece type get a bitboard that encodes the squares occupied by that piece type
	Bitboard attackingBishops = GetPieceBB(pos, BISHOP) | GetPieceBB(pos, QUEEN);
	Bitboard attackingRooks = GetPieceBB(pos, ROOK) | GetPieceBB(pos, QUEEN);

	return (pawn_attacks[WHITE][to] & GetPieceColorBB(pos, PAWN, BLACK))
		| (pawn_attacks[BLACK][to] & GetPieceColorBB(pos, PAWN, WHITE))
		| (knight_attacks[to] & GetPieceBB(pos, KNIGHT))
		| (king_attacks[to] & GetPieceBB(pos, KING))
		| (get_bishop_attacks(to, occ) & attackingBishops)
		| (get_rook_attacks(to, occ) & attackingRooks);

}

// inspired by the Weiss engine
bool SEE(const S_Board* pos, const int move,
	const int threshold) {
	int to = To(move);
	int from = From(move);

	int target = PieceOn(pos, to);
	// Making the move and not losing it must beat the threshold
	int value = PieceValue[target] - threshold;

	if (get_move_promoted(move)) return true;

	if (value < 0)
		return false;

	int attacker = PieceOn(pos, from);
	// Trivial if we still beat the threshold after losing the piece
	value -= PieceValue[attacker];

	if (value >= 0)
		return true;

	// It doesn't matter if the to square is occupied or not
	Bitboard occupied = Occupancy(pos, BOTH) ^ (1ULL << from) ^ (1ULL << to);
	Bitboard attackers = AttacksTo(pos, to, occupied);

	Bitboard bishops = GetPieceBB(pos, BISHOP) | GetPieceBB(pos, QUEEN);
	Bitboard rooks = GetPieceBB(pos, ROOK) | GetPieceBB(pos, QUEEN);

	int side = !Color[attacker];

	// Make captures until one side runs out, or fail to beat threshold
	while (true) {

		// Remove used pieces from attackers
		attackers &= occupied;

		Bitboard myAttackers = attackers & Occupancy(pos, side);
		if (!myAttackers) {

			break;
		}


		// Pick next least valuable piece to capture with
		int pt;
		for (pt = PAWN; pt < KING; ++pt) {
			if (myAttackers & GetPieceBB(pos, pt))
				break;
		}

		side = !side;

		value = -value - 1 - PieceValue[pt];

		// Value beats threshold, or can't beat threshold (negamaxed)
		if (value >= 0) {

			if (pt == KING && (attackers & Occupancy(pos, side)))
				side = !side;

			break;
		}
		// Remove the used piece from occupied
		occupied ^= (1ULL << (get_ls1b_index(myAttackers & GetPieceBB(pos, pt))));


		if (pt == PAWN || pt == BISHOP || pt == QUEEN)
			attackers |= get_bishop_attacks(to, occupied) & bishops;
		if (pt == ROOK || pt == QUEEN)
			attackers |= get_rook_attacks(to, occupied) & rooks;
	}

	return side != Color[attacker];
}

// score_moves takes a list of move as an argument and assigns a score to each move
static inline void score_moves(S_Board* pos, S_Stack* ss, S_MOVELIST* move_list,
	int PvMove) {
	//Loop through all the move in the movelist
	for (int i = 0; i < move_list->count; i++) {
		int move = move_list->moves[i].move;
		//If the move is from the TT (aka it's our hashmove) give it the highest score
		if (move == PvMove) {
			move_list->moves[i].score = INT32_MAX - 100;
			continue;
		}
		else if (get_move_promoted(move)) {
			move_list->moves[i].score = 2000000000 + get_move_promoted(move);
			continue;
		}
		//if the move is an enpassant or a promotion give it a score that a good capture of type pawn-pwan would have
		else if (isEnpassant(pos, move)) {
			move_list->moves[i].score = 105 + goodCaptureScore;
			continue;
		}
		//if the mvoe is a capture sum the mvv-lva score to a variable that depends on whether the capture has a positive SEE or not 
		else if (get_move_capture(move)) {
			move_list->moves[i].score =
				mvv_lva[get_move_piece(move)][PieceOn(pos, To(move))] +
				goodCaptureScore * SEE(pos, move, -107);
			continue;
		}
		//First  killer move always comes after the TT move,the promotions and the good captures and before anything else
		else if (ss->searchKillers[0][pos->ply] == move) {
			move_list->moves[i].score = killerMoveScore0;
			continue;
		}
		//Second killer move always comes after the first one
		else if (ss->searchKillers[1][pos->ply] == move) {
			move_list->moves[i].score = killerMoveScore1;
			continue;
		}
		//After the killer moves try the Counter moves
		else if (move == CounterMoves[From(pos->history[pos->hisPly].move)][To(pos->history[pos->hisPly].move)])
		{
			move_list->moves[i].score = 600000000;
			continue;
		}
		//if the move isn't in any of the previous categories score it according to the history heuristic
		else {
			move_list->moves[i].score = getHHScore(pos, ss, move);
			continue;
		}
	}

	return;
}

//Calculate a futility margin based on depth and if the search is improving or not
int futility(int depth, bool improving) { return 66 * (depth - improving); }

//Quiescence search to avoid the horizon effect
int Quiescence(int alpha, int beta, S_Board* pos, S_Stack* ss, S_SearchINFO* info) {
	// Initialize the node
	bool pv_node = (beta - alpha) > 1;
	//tte is an hashtable entry, it will store the values fetched from the TT
	S_HASHENTRY tte;
	bool TThit = false;
	int standing_pat = 0;

	// check if time is up or we searched the maximum number of nodes we could search
	if ((info->timeset == true && GetTimeMs() > info->stoptimeMax)
		|| (info->nodeset == true && info->nodes > info->nodeslimit)) {
		info->stopped = true;
	}
	//Check for the highest depth reached in search to report it to the cli
	if (pos->ply > info->seldepth)
		info->seldepth = pos->ply;

	//If position is a draw return a randomized draw score to avoid 3-fold blindness
	if (IsDraw(pos)) {
		return 1 - (info->nodes & 2);
	}

	//If we reached maxdepth we return a static evaluation of the position
	if (pos->ply > MAXDEPTH - 1) {
		return EvalPosition(pos);
	}

	//Get a static evaluation of the position
	standing_pat = EvalPosition(pos);

	alpha = (std::max)(alpha, standing_pat);

	if (standing_pat >= beta) return standing_pat;

	//TThit is true if and only if we find something in the TT
	TThit = ProbeHashEntry(pos, alpha, beta, 0, &tte);

	//If we found a value in the TT we can return it
	if (!pv_node && TThit) {
		if ((tte.flags == HFALPHA && tte.score <= alpha) || (tte.flags == HFBETA && tte.score >= beta) || (tte.flags == HFEXACT))
			return tte.score;
	}

	// create move list instance
	S_MOVELIST move_list[1];

	// generate the captures
	generate_captures(move_list, pos);

	//score the generated moves
	score_moves(pos, ss, move_list, tte.move);

	//set up variables needed for the search
	int BestScore = standing_pat;
	int bestmove = NOMOVE;
	int Score = -MAXSCORE;

	// old value of alpha
	int old_alpha = alpha;

	int moves_searched = 0;

	// loop over moves within a movelist
	for (int count = 0; count < move_list->count; count++) {
		pick_move(move_list, count);
		int move = move_list->moves[count].move;
		int score = move_list->moves[count].score;
		// See pruning
		if (score < goodCaptureScore
			&& moves_searched >= 1)
		{
			continue;
		}
		make_move(move, pos);
		// increment nodes count
		info->nodes++;
		//Call Quiescence search recursively
		Score = -Quiescence(-beta, -alpha, pos, ss, info);

		// take move back
		Unmake_move(pos);

		if (info->stopped)
			return 0;

		moves_searched++;

		//If the Score of the current move is the best we've found until now
		if (Score > BestScore) {
			//Update the best move found and what the best score is
			BestScore = Score;

			// if the Score is better than alpha update alpha
			if (Score > alpha) {
				alpha = Score;
				bestmove = move;

				// if the Score is better than beta save the move in the TT and return beta
				if (Score >= beta) break;
			}
		}
	}
	//Set the TT flag based on whether the BestScore is better than alpha and if not based on if we changed alpha or not

	int flag = BestScore >= beta ? HFBETA : (alpha != old_alpha) ? HFEXACT : HFALPHA;

	StoreHashEntry(pos, bestmove, BestScore, flag, 0, pv_node);

	// node (move) fails low
	return BestScore;
}

//Calculate a reduction margin based on the search depth and the number of moves played
static inline int reduction(bool pv_node, bool improving, int depth, int num_moves) {
	return  reductions[depth] * reductions[num_moves] + !improving + !pv_node;
}

// negamax alpha beta search
int negamax(int alpha, int beta, int depth, S_Board* pos, S_Stack* ss, S_SearchINFO* info,
	int DoNull) {
	// Initialize the node
	bool in_check = IsInCheck(pos, pos->side);
	S_MOVELIST quiet_moves;
	quiet_moves.count = 0;
	int root_node = (pos->ply == 0);
	int eval, static_eval;
	bool improving;
	bool ttHit;
	int Score = -MAXSCORE;
	S_HASHENTRY tte;
	int pv_node = (beta - alpha) > 1;
	bool SkipQuiets = false;
	int excludedMove = ss->excludedMoves[pos->ply];

	ss->pvLength[pos->ply] = pos->ply;

	if (in_check) depth = (std::max)(1, depth + 1);

	//Check for the highest depth reached in search to report it to the cli
	if (pos->ply > info->seldepth)
		info->seldepth = pos->ply;

	// recursion escape condition
	if (depth <= 0) {
		return Quiescence(alpha, beta, pos, ss, info);
	}

	// check if time is up or we searched the maximum number of nodes we could search
	if ((info->timeset == TRUE && GetTimeMs() > info->stoptimeMax)
		|| (info->nodeset == TRUE && info->nodes > info->nodeslimit)) {
		info->stopped = true;
	}

	if (!root_node) {
		//If position is a draw return a randomized draw score to avoid 3-fold blindness
		if (IsDraw(pos)) {
			return 8 - (info->nodes & 7);
		}

		//If we reached maxdepth we return a static evaluation of the position
		if (pos->ply > MAXDEPTH - 1) {
			return EvalPosition(pos);
		}

		// Mate distance pruning
		int mating_value = mate_value - pos->ply;

		if (mating_value < beta) {
			beta = mating_value;
			if (alpha >= mating_value)
				return mating_value;
		}
	}

	ttHit = excludedMove ? false : ProbeHashEntry(pos, alpha, beta, depth, &tte);
	//If we found a value in the TT we can return it
	if (!pv_node
		&& ttHit
		&& tte.depth >= depth) {
		if ((tte.flags == HFALPHA && tte.score <= alpha) || (tte.flags == HFBETA && tte.score >= beta) || (tte.flags == HFEXACT))
			return tte.score;
	}

	// IIR by Ed Schroder (That i find out about in Berserk source code)
	// http://talkchess.com/forum3/viewtopic.php?f=7&t=74769&sid=64085e3396554f0fba414404445b3120
	// https://github.com/jhonnold/berserk/blob/dd1678c278412898561d40a31a7bd08d49565636/src/search.c#L379
	if (depth >= 4 && !tte.move && !excludedMove)
		depth--;

	if (in_check || excludedMove) {
		static_eval = value_none;
		pos->history[pos->hisPly].eval = value_none;
		improving = false;
		goto moves_loop; //if we are in check we jump directly to the move loop because the net isn't good when evaluating positions that are in check
	}

	// get static evaluation score
	static_eval = eval = EvalPosition(pos);
	pos->history[pos->hisPly].eval = static_eval;

	//if we aren't in check and the eval of this position is better than the position of 2 plies ago (or we were in check 2 plies ago), it means that the position is "improving" this is later used in some forms of pruning
	improving = (pos->hisPly >= 2) &&
		(static_eval > (pos->history[pos->hisPly - 2].eval) ||
			(pos->history[pos->hisPly - 2].eval) == value_none);

	if (!pv_node) {

		//if we have a TThit we can use the search score as a more accurate form of eval
		if (ttHit) {
			eval = tte.score;
		}

		// Reverse futility pruning 
		if (depth < 9
			&& eval - futility(depth, improving) >= beta)
			return eval;

		// null move pruning: If we can give our opponent a free move and still be above beta after a reduced search we can return beta, we check if the board has non pawn pieces to avoid zugzwangs
		if (DoNull
			&& static_eval >= beta
			&& eval >= beta
			&& pos->ply
			&& depth >= 3
			&& BoardHasNonPawns(pos, pos->side)) {
			MakeNullMove(pos);
			int R = 3 + depth / 3;
			/* search moves with reduced depth to find beta cutoffs
			   depth - 1 - R where R is a reduction limit */
			Score = -negamax(-beta, -beta + 1, depth - R, pos, ss, info, FALSE);

			TakeNullMove(pos);

			if (info->stopped)
				return 0;

			// fail-hard beta cutoff
			if (Score >= beta && abs(Score) < ISMATE)
				// node (position) fails high
				return beta;
		}

		// razoring
		if ((depth <= 3) &&
			(eval + 119 + 182 * (depth - 1) <= alpha))
		{
			return Quiescence(alpha, beta, pos, ss, info);
		}

	}

moves_loop:
	// create move list instance
	S_MOVELIST move_list[1];

	// generate moves
	generate_moves(move_list, pos);

	score_moves(pos, ss, move_list, tte.move);

	// old value of alpha
	int old_alpha = alpha;
	int BestScore = -MAXSCORE;
	int bestmove = NOMOVE;

	int moves_searched = 0;

	// loop over moves within a movelist
	for (int count = 0; count < move_list->count; count++) {
		pick_move(move_list, count);
		//get the move with the highest score in the move ordering
		int move = move_list->moves[count].move;
		if (move == excludedMove) continue;
		bool isQuiet = IsQuiet(move);

		if (isQuiet && SkipQuiets) continue;

		//if the move isn't a quiet move we update the quiet moves list and counter
		if (isQuiet) {
			quiet_moves.moves[quiet_moves.count].move = move;
			quiet_moves.count++;
		}

		if (!root_node)
		{
			//Movecount pruning: if we searched enough quiet moves and we are not in check we skip the others
			if (!pv_node
				&& !in_check
				&& depth < 4
				&& isQuiet
				&& (quiet_moves.count > (depth * 8))) {
				SkipQuiets = true;
				continue;
			}

			// See pruning
			if (depth <= 8
				&& moves_searched >= 2
				&& !SEE(pos, move, -50 * depth))
			{
				continue;
			}
		}

		int extension = 0;

		if (!root_node
			&& depth >= 7
			&& move == tte.move
			&& !excludedMove
			&& (tte.flags & HFBETA)
			&& abs(tte.score) < ISMATE
			&& tte.depth >= depth - 3)
		{
			int singularBeta = tte.score - 3 * depth;
			int singularDepth = (depth - 1) / 2;

			ss->excludedMoves[pos->ply] = tte.move;
			int singularScore = negamax(singularBeta - 1, singularBeta, singularDepth, pos, ss, info, false);
			ss->excludedMoves[pos->ply] = NOMOVE;

			if (singularScore < singularBeta)
				extension = 1;

			else if (singularBeta >= beta)
				return (singularBeta);

		}
		//we adjust the search depth based on potential extensions
		int newDepth = depth + extension;
		//Play the move
		make_move(move, pos);
		//Speculative prefetch of the TT entry
		prefetch(&HashTable->pTable[(pos->posKey) % HashTable->numEntries]);
		// increment nodes count
		info->nodes++;

		// full depth search
		if (moves_searched == 0)
			// do normal alpha beta search
			Score = -negamax(-beta, -alpha, newDepth - 1, pos, ss, info, TRUE);

		// late move reduction: After we've searched /full_depth_moves/ and if we are at an appropriate depth we can search the remaining moves at a reduced depth
		else {
			// condition to consider LMR
			if (moves_searched >= 5
				&& depth >= 3
				&& !in_check
				&& IsQuiet(move))
			{
				//calculate by how much we should reduce the search depth 
				int depth_reduction = reduction(pv_node, improving, depth, moves_searched);

				// search current move with reduced depth:
				Score = -negamax(-alpha - 1, -alpha, newDepth - depth_reduction, pos, ss, info,
					TRUE);
			}

			// hack to ensure that full-depth search is done
			else
				Score = alpha + 1;

			// principle variation search PVS
			if (Score > alpha) {
				Score = -negamax(-alpha - 1, -alpha, newDepth - 1, pos, ss, info, TRUE);

				if ((Score > alpha) && (Score < beta))

					Score = -negamax(-beta, -alpha, newDepth - 1, pos, ss, info, TRUE);
			}
		}

		// take move back
		Unmake_move(pos);

		if (info->stopped)
			return 0;

		moves_searched++;
		//If the Score of the current move is the best we've found until now
		if (Score > BestScore) {
			//Update the best move found and what the best score is
			BestScore = Score;

			// found a better move
			if (Score > alpha) {
				bestmove = move;
				alpha = Score;
				//Update the pv table
				ss->pvArray[pos->ply][pos->ply] = move;
				for (int next_ply = pos->ply + 1;next_ply < ss->pvLength[pos->ply + 1];next_ply++)
				{
					ss->pvArray[pos->ply][next_ply] = ss->pvArray[pos->ply + 1][next_ply];
				}
				ss->pvLength[pos->ply] = ss->pvLength[pos->ply + 1];

				if (Score >= beta)
				{
					//If the move that caused the beta cutoff is quiet we have a killer move
					if (IsQuiet(move)) {
						//Don't update killer moves if it would result in having 2 identical killer moves
						if (ss->searchKillers[0][pos->ply] != bestmove) {
							// store killer moves
							ss->searchKillers[1][pos->ply] = ss->searchKillers[0][pos->ply];
							ss->searchKillers[0][pos->ply] = bestmove;
						}

						//Save CounterMoves
						int previousMove = pos->history[pos->hisPly].move;
						CounterMoves[From(previousMove)][To(previousMove)] = move;
						//Update the history heuristic based on the new best move
						updateHH(pos, ss, depth, bestmove, &quiet_moves);
					}
					// node (move) fails high
					break;
				}
			}
		}
	}

	// we don't have any legal moves to make in the current postion
	if (move_list->count == 0) {
		// if the king is in check return mating score (assuming closest distance to mating position) otherwise return stalemate 
		BestScore = excludedMove ? alpha : in_check ? (-mate_value + pos->ply) : 0;
	}
	//Set the TT flag based on whether the BestScore is better than beta and if not based on if we changed alpha or not

	int flag = BestScore >= beta ? HFBETA : (alpha != old_alpha) ? HFEXACT : HFALPHA;

	if (!excludedMove) StoreHashEntry(pos, bestmove, BestScore, flag, depth, pv_node);
	// node (move) fails low
	return BestScore;
}

//Starts the search process, this is ideally the point where you can start a multithreaded search
void Root_search_position(int depth, S_Board* pos, S_Stack* ss, S_SearchINFO* info, S_UciOptions* options) {
	if (options->datagen) datagen(pos, ss, info);
	else search_position(1, depth, pos, ss, info, options);
}

// search_position is the actual function that handles the search, it sets up the variables needed for the search , calls the negamax function and handles the console output
void search_position(int start_depth, int final_depth, S_Board* pos, S_Stack* ss,
	S_SearchINFO* info, S_UciOptions* options) {
	//variable used to store the score of the best move found by the search (while the move itself can be retrieved from the TT)
	int score = 0;

	//Clean the position and the search info to start search from a clean state 
	ClearForSearch(pos, ss, info);

	// Call the negamax function in an iterative deepening framework
	for (int current_depth = start_depth; current_depth <= final_depth; current_depth++)
	{
		score = aspiration_window_search(score, current_depth, pos, ss, info);

		// check if we just cleared a depth and more than OptTime passed
		if ((info->timeset && GetTimeMs() > info->stoptimeOpt)
			|| (info->nodeset == TRUE && info->nodes > info->nodeslimit))
			info->stopped = true;

		if (info->stopped)
			// stop calculating and return best move so far
			break;
		PrintUciOutput(score, current_depth, info, options);

		// loop over the moves within a PV line
		for (int count = 0; count < ss->pvLength[0]; count++) {
			// print PV move
			print_move(ss->pvArray[0][count]);
			printf(" ");
		}

		// print new line
		printf("\n");
	}

	printf("bestmove ");
	print_move(getBestMove(ss));
	printf("\n");
}


int  getBestMove(S_Stack* ss) {
	return ss->pvArray[0][0];
}

int aspiration_window_search(int prev_eval, int depth, S_Board* pos, S_Stack* ss, S_SearchINFO* info) {
	int score = 0;
	//We set an expected window for the score at the next search depth, this window is not 100% accurate so we might need to try a bigger window and re-search the position, resize counter keeps track of how many times we had to re-search
	int alpha_window = -17;
	int resize_counter = 0;
	int beta_window = 17;
	// define initial alpha beta bounds
	int alpha = -MAXSCORE;
	int beta = MAXSCORE;

	// only set up the windows is the search depth is bigger or equal than Aspiration_Depth to avoid using windows when the search isn't accurate enough
	if (depth >= 3) {
		alpha = prev_eval + alpha_window;
		beta = prev_eval + beta_window;
	}

	//Stay at current depth if we fail high/low because of the aspiration windows
	while (true) {

		score = negamax(alpha, beta, depth, pos, ss, info, TRUE);

		// check if we just cleared a depth and more than OptTime passed
		if ((info->timeset && GetTimeMs() > info->stoptimeOpt)
			|| (info->nodeset == TRUE && info->nodes > info->nodeslimit))
			info->stopped = true;

		if (info->stopped)
			// stop calculating and return best move so far
			break;

		// we fell outside the window, so try again with a bigger window for up to Resize_limit times, if we still fail after we just search with a full window
		if ((score <= alpha)) {
			if (resize_counter > 5)
				alpha = -MAXSCORE;
			beta = (alpha + beta) / 2;
			alpha_window *= 1.44;
			alpha += alpha_window + 1;
			resize_counter++;
		}

		// we fell outside the window, so try again with a bigger window for up to Resize_limit times, if we still fail after we just search with a full window
		else if ((score >= beta)) {
			if (resize_counter > 5)
				beta = MAXSCORE;
			beta_window *= 1.44;
			beta += beta_window + 1;
			resize_counter++;
		}
		else break;

	}
	return score;
}